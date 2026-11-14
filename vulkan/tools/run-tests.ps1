# Runs every automated check with one command. Used by CI and after a change.
#
#   pwsh -File tools/run-tests.ps1
#
# Covers:
#   1. the build (shaders and C++)
#   2. batch smoke test: ten thousand sprites for 120 frames without an error
#   3. pixel assertions: projection / texture / blend / scissor / colour byte order
param(
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$failures = 0

# A crashed previous run may leave a demo process behind (waiting on a dialog,
# for example), which keeps bin/lime-vulkan-demo.exe locked and the next link fails
# with LNK1104.
Get-Process lime-vulkan-demo, lime-vulkan-attach -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 200

function Step([string]$name, [scriptblock]$body) {
    Write-Host ''
    Write-Host "=== $name ==="
    try {
        & $body
        if ($LASTEXITCODE -ne 0 -and $null -ne $LASTEXITCODE) { throw "exit code $LASTEXITCODE" }
        Write-Host "[pass] $name"
    } catch {
        $script:failures++
        Write-Host "[FAIL] $name : $_"
    }
}

Step 'build' {
    if ($SkipBuild) { Write-Host '[skip] -SkipBuild'; return }
    & pwsh -NoProfile -File (Join-Path $root 'build-native.ps1')
}

Step 'batch-smoke' {
    $text = (& (Join-Path $root 'bin/lime-vulkan-demo.exe') --scene=batch --quads=10000 --frames=120 --no-dialog 2>&1 | Out-String)
    ($text.Trim() -split "\r?\n" | Select-Object -Last 4) | Write-Host
    if ($text -match 'error=[^\s]+') { throw "frame reported an error: $($matches[0])" }
    if ($text -notmatch 'frames=120') { throw 'did not reach 120 frames' }
}

Step 'batch-multitile' {
    # Beyond one flush worth of capacity, which exercises the growth path
    $text = (& (Join-Path $root 'bin/lime-vulkan-demo.exe') --scene=batch --quads=50000 --frames=90 --no-dialog 2>&1 | Out-String)
    ($text.Trim() -split "\r?\n" | Select-Object -Last 4) | Write-Host
    if ($text -notmatch 'frames=90') { throw 'did not reach 90 frames' }
}

Step 'shader-toolchain' {
    # Translation, compilation, std140 offsets and SPIR-V alignment, disk cache
    & python (Join-Path $root 'tests/test_shader_toolchain.py') | Write-Host
}

Step 'library-exports' {
    # The renderer lives inside Lime.ndll: every primitive the Haxe side loads must be exported
    & pwsh -NoProfile -File (Join-Path $root 'tests/check_exports.ps1') | Write-Host
}

Step 'game-frame' {
    # Acceptance on the real game process: GL frame vs Vulkan frame (needs a game build)
    $game = Join-Path $root '..\FNF-SeiunEngine'
    if (-not (Test-Path (Join-Path $game 'export\debug\windows\bin\SeiunEngine.exe'))) {
        Write-Host '[skip] the game is not built'
        return
    }
    & python (Join-Path $root 'tests/test_game_frame.py') | Write-Host
}

Step 'quad-batches' {
    # flixel shaped batches: view matrix times quad matrix, uv normalisation, per-vertex alpha and multipliers
    & python (Join-Path $root 'tests/test_quad_batches.py') | Write-Host
}

Step 'shader-pixels' {
    # Pixel assertions for five representative shaders (each writes its own screenshot)
    & python (Join-Path $root 'tests/test_shader_pixels.py') | Write-Host
}

Step 'shader-perf' {
    # Ten thousand sprites through a custom shader: has to finish and keep the frame rate up
    $text = (& (Join-Path $root 'bin/lime-vulkan-demo.exe') --scene=shader --vert=tests/shaders/sprite.vert --frag=tests/shaders/warp.frag --sprites=10000 --frames=180 --no-dialog --cache=build/shadercache --uniform=uArray=0.6,0.4,0.3,0.2 2>&1 | Out-String)
    ($text.Trim() -split "\r?\n" | Select-Object -Last 4) | Write-Host
    if ($text -match 'error=[^\s]+') { throw "frame reported an error: $($matches[0])" }
    if ($text -notmatch 'frames=180') { throw 'did not reach 180 frames' }

    $rates = [regex]::Matches($text, '(\d+) fps') | ForEach-Object { [int]$_.Groups[1].Value }
    $best = ($rates | Measure-Object -Maximum).Maximum
    if ($best -lt 60) { throw "steady state fell to $best fps" }
    Write-Host "  steady state: $best fps"
}

Step 'pixels' {
    $shot = Join-Path $root 'logs/screenshots/test.png'
    & pwsh -NoProfile -File (Join-Path $root 'tools/capture-demo.ps1') -DemoArguments '--scene=test' -Out $shot -DelayMs 1200 | Write-Host
    & python (Join-Path $root 'tests/test_batch_pixels.py') $shot | Write-Host
}

Write-Host ''
if ($failures -gt 0) {
    Write-Host "[result] $failures check(s) failed"
    exit 1
}

Write-Host '[result] all checks passed'
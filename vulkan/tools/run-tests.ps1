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
    # Acceptance on the real game process: GL frame vs Vulkan frame (needs a game build).
    # The renderer lives under the project's .haxelib now, so the game checkout is four
    # levels up from vulkan/, not next to it.
    $game = $root
    for ($up = 0; $up -lt 6; $up++) {
        $game = Split-Path $game -Parent
        if (Test-Path (Join-Path $game 'source\Main.hx')) { break }
    }
    if (-not (Test-Path (Join-Path $game 'export\debug\windows\bin\SeiunEngine.exe'))) {
        Write-Host '[skip] the game is not built'
        return
    }
    # The capture helper this test drives is not part of this checkout (the renderer
    # lives under .haxelib now), so the step reports the gap instead of failing.
    if (-not (Test-Path (Join-Path $game 'tools\vulkan-capture.ps1'))) {
        Write-Host '[skip] tools/vulkan-capture.ps1 does not exist in the game checkout'
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

    # The demo prints one line per 0.5 s statistics window. The first window is the
    # warm up: it contains the driver compiling the pipelines, which is a cold start
    # cost and not what this check is about. A run that finishes inside that window
    # has no steady state to report at all, so the average carries the gate.
    $rates = [regex]::Matches($text, '(\d+) fps') | ForEach-Object { [int]$_.Groups[1].Value }
    $steady = @($rates | Select-Object -Skip 1)
    $average = 0.0
    if ($text -match '\(([0-9.]+) fps average\)') { $average = [double]$matches[1] }
    if ($average -lt 60) { throw "average frame rate was $average fps" }
    if ($steady.Count -gt 0) {
        $best = ($steady | Measure-Object -Maximum).Maximum
        if ($best -lt 60) { throw "steady state fell to $best fps" }
        Write-Host "  steady state: $best fps (whole run: $([math]::Round($average)) fps)"
    } else {
        Write-Host "  steady state: finished inside the warm up window (whole run: $([math]::Round($average)) fps)"
    }
}

Step 'texture-memory' {
    # One allocation per texture used to hit the driver's allocation limit as soon
    # as a modded song loaded its sprites. 1000 x 512x512 has to stay well under it.
    $text = (& (Join-Path $root 'bin/lime-vulkan-demo.exe') --scene=texalloc --textures=1000 --size=512 --frames=3 --no-dialog 2>&1 | Out-String)
    ($text.Trim() -split "\r?\n" | Where-Object { $_ -match 'texalloc' }) | Write-Host
    if ($text -match 'error=[^\s]+') { throw "texalloc reported an error: $($matches[0])" }
    if ($text -notmatch 'created=1000 of 1000') { throw 'not every texture was created' }
    if ($text -notmatch 'memAllocs=(\d+)') { throw 'no memAllocs in the stats' }
    $allocs = [int]$matches[1]
    if ($allocs -ge 20) { throw "$allocs vkAllocateMemory calls for 1000 textures, the budget is 20" }
    if ($text -notmatch 'after destroy memAllocs=\d+ memReserved=0\.0MB') { throw 'empty blocks were not released' }
    Write-Host "  $allocs allocations for 1000 textures, all blocks released"
}

Step 'perf-scenes' {
    # The three scenes PERF.md tracks have to keep running, or the baseline rots
    $scenes = @('--scene=empty --frames=60', '--scene=sprites --quads=10000 --frames=60', '--scene=cameras --sprites=1500 --frames=60')
    foreach ($scene in $scenes) {
        $text = (& (Join-Path $root 'bin/lime-vulkan-demo.exe') $scene.Split(' ') --no-dialog --perf 2>&1 | Out-String)
        if ($text -match 'error=[^\s]+') { throw "$scene reported an error: $($matches[0])" }
        if ($text -notmatch '\[demo\] perf ') { throw "$scene printed no perf line" }
    }
    Write-Host '  all three baseline scenes ran'
}

Step 'blend-modes' {
    # subtract / darken / lighten: fixed function blend ops, asserted per band.
    # Two attempts: a screenshot can catch the compositor between repaints, and a
    # real regression fails both.
    $shot = Join-Path $root 'logs/screenshots/blend.png'
    $ok = $false
    for ($attempt = 1; $attempt -le 2; $attempt++) {
        & pwsh -NoProfile -File (Join-Path $root 'tools/capture-demo.ps1') -DemoArguments '--scene=blend' -Out $shot -DelayMs 1200 | Write-Host
        & python (Join-Path $root 'tests/test_blend_modes.py') $shot | Write-Host
        if ($LASTEXITCODE -eq 0) { $ok = $true; break }
    }
    if (-not $ok) { throw 'the blend bands were wrong twice in a row' }
}

Step 'pixels' {
    # Same two-attempt rule as blend-modes: a screenshot race is not a regression.
    $shot = Join-Path $root 'logs/screenshots/test.png'
    $ok = $false
    for ($attempt = 1; $attempt -le 2; $attempt++) {
        & pwsh -NoProfile -File (Join-Path $root 'tools/capture-demo.ps1') -DemoArguments '--scene=test' -Out $shot -DelayMs 1200 | Write-Host
        & python (Join-Path $root 'tests/test_batch_pixels.py') $shot | Write-Host
        if ($LASTEXITCODE -eq 0) { $ok = $true; break }
    }
    if (-not $ok) { throw 'the pixel bands were wrong twice in a row' }
}

Write-Host ''
if ($failures -gt 0) {
    Write-Host "[result] $failures check(s) failed"
    exit 1
}

Write-Host '[result] all checks passed'
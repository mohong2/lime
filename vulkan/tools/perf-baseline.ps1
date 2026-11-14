# Baseline measurement for the three scenes PERF.md tracks.
#
#   pwsh -File tools/perf-baseline.ps1                 # 300 frames per scene
#   pwsh -File tools/perf-baseline.ps1 -Frames 900
#   pwsh -File tools/perf-baseline.ps1 -Runs 3         # take the best of three
#
# Prints one markdown table row per scene:
#
#   | scene | frames | cpu p50/p95 ms | gpu p50/p95 ms | draws | upload MB |
#
# The numbers describe the same three shapes every time, so a change can be
# compared instead of "it feels faster now". The history ring in the renderer
# keeps the last 240 frames, so a 300 frame run has already dropped the warm up.
param(
    [int]$Frames = 300,
    [int]$Runs = 1
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$demo = Join-Path $root 'bin/lime-vulkan-demo.exe'

if (-not (Test-Path $demo)) {
    throw 'build the demo first: pwsh -File build-native.ps1 -Targets all'
}

$scenes = @(
    @{ name = 'empty';   args = "--scene=empty --frames=$Frames" },
    @{ name = 'sprites'; args = "--scene=sprites --quads=10000 --frames=$Frames" },
    @{ name = 'cameras'; args = "--scene=cameras --sprites=1500 --frames=$Frames" }
)

function Value([string]$text, [string]$key) {
    if ($text -match "$key=([0-9.]+)") { return [double]$matches[1] }
    return -1
}

Write-Host '| scene | frames | cpu p50/p95 | gpu p50/p95 | draws | upload MB |'
Write-Host '|---|---|---|---|---|---|'

foreach ($scene in $scenes) {

    $best = $null

    for ($run = 1; $run -le $Runs; $run++) {

        $text = (& $demo $scene.args.Split(' ') --no-dialog --perf 2>&1 | Out-String)

        if ($text -match 'error=[^\s]+') {
            throw "$($scene.name): the demo reported an error: $($matches[0])"
        }

        $line = (($text -split "\r?\n") | Where-Object { $_ -match '\[demo\] perf ' }) -join ''

        if (-not $line) { throw "$($scene.name): no perf line, did the run finish?" }

        $entry = @{
            frames = Value $line 'frames'
            cpuP50 = Value $line 'cpuP50'
            cpuP95 = Value $line 'cpuP95'
            gpuP50 = Value $line 'gpuP50'
            gpuP95 = Value $line 'gpuP95'
            draws  = Value $line 'draws'
            upload = Value $line 'uploadMB'
        }

        if ($null -eq $best -or $entry.cpuP95 -lt $best.cpuP95) { $best = $entry }

    }

    $row = '| {0} | {1} | {2} / {3} | {4} / {5} | {6} | {7} |' -f $scene.name, $best.frames, $best.cpuP50, $best.cpuP95, $best.gpuP50, $best.gpuP95, $best.draws, $best.upload

    Write-Host $row

}

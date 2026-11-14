# Confirms that Lime.ndll really carries the renderer.
#
# The Haxe side loads every entry point by name at runtime, so a missing export
# is invisible until the game asks for Vulkan. This check reads the primitive
# names out of VkExtern.hx, derives the exported symbol names (hxcpp appends
# __<nargs>) and looks for them in the shared library.

param(
    [string]$Ndll
)

$ErrorActionPreference = 'Stop'

$root = Split-Path $PSScriptRoot -Parent
if (-not $Ndll) { $Ndll = Join-Path $root '../ndll/Windows64/lime.ndll' }

if (-not (Test-Path $Ndll)) {
    throw "Lime.ndll not found: $Ndll. Build it with 'lime rebuild windows'."
}

$binding = Get-Content -LiteralPath (Join-Path $root 'haxe/seiun/vulkan/VkExtern.hx') -Raw
$expected = @(
    foreach ($match in [regex]::Matches($binding, 'load \("([a-z0-9_]+)", (\d+)\)')) {
        '{0}__{1}' -f $match.Groups[1].Value, $match.Groups[2].Value
    }
)

if ($expected.Count -lt 40) {
    throw "only $($expected.Count) primitives found in VkExtern.hx, the pattern probably changed"
}

# hxcpp resolves a primitive with at most five arguments: __loadprim appends "__MULT"
# for anything above that and the lookup then fails at runtime, with the error only
# showing up as a null call inside the game. Fail the build check instead.
$tooMany = @(
    foreach ($match in [regex]::Matches($binding, 'load ("([a-z0-9_]+)", (d+))')) {
        if ([int]$match.Groups[2].Value -gt 5) {
            '{0} takes {1} arguments' -f $match.Groups[1].Value, $match.Groups[2].Value
        }
    }
)

if ($tooMany.Count -gt 0) {
    Write-Host 'primitives over the hxcpp limit:'
    $tooMany | ForEach-Object { Write-Host "  $_" }
    throw "$($tooMany.Count) primitive(s) take more than five arguments and can never bind"
}

# The C entry points the tools and the pixel tests call directly.
$expected += @('lime_vulkan_version', 'lime_vulkan_texture_create', 'lime_vulkan_batch_quads_d')
# Android only, but defined on every platform so the ABI (and this check) stays one list.
$expected += @('lime_vulkan_set_android_window')

$bytes = [System.IO.File]::ReadAllBytes($Ndll)
$text = [System.Text.Encoding]::Latin1.GetString($bytes)

$missing = @($expected | Where-Object { -not $text.Contains($_) })

if ($missing.Count -gt 0) {
    Write-Host "missing exports ($($missing.Count)):"
    $missing | Select-Object -First 20 | ForEach-Object { Write-Host "  $_" }
    throw "$($missing.Count) of $($expected.Count) symbols are not in $Ndll"
}

Write-Host "$($expected.Count) symbols present in $(Split-Path $Ndll -Leaf) ($([math]::Round($bytes.Length / 1MB, 1)) MB)"

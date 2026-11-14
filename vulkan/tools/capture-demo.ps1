# Starts the demo, takes one screenshot, then closes it.
# Purpose: proof that Vulkan really draws (for CI and for a hand-over).
#
#   pwsh -File tools/capture-demo.ps1
#   pwsh -File tools/capture-demo.ps1 -DemoArguments '--scene=batch --quads=10000' -Out logs/screenshots/batch.png
#
# The capture is taken twice and only accepted once two shots 200 ms apart are
# identical: a compositor fade in, a window that is still moving and a frame the
# GPU has not presented yet all produce a screenshot with plausible looking but
# wrong pixels, which is the worst kind of failure for a pixel assertion.
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\bin\lime-vulkan-demo.exe'),
    [string]$Out = (Join-Path $PSScriptRoot '..\logs\screenshots\demo.png'),
    [string]$DemoArguments = '',
    [int]$DelayMs = 1500
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System;
using System.Runtime.InteropServices;
public class Win32Capture {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT lpPoint);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hWnd, int command);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
"@

# One window at a time. A demo left over from an earlier step would sit on top of
# this capture (or push the new window into a cascade position), and the result
# would be a screenshot of the wrong scene that still looks plausible.
$stale = Get-Process -Name ([System.IO.Path]::GetFileNameWithoutExtension($Exe)) -ErrorAction SilentlyContinue

foreach ($leftover in $stale) {
    try {
        $leftover.Kill()
        [void]$leftover.WaitForExit(3000)
    } catch {}
}

Start-Sleep -Milliseconds 250

if ($DemoArguments) {
    $process = Start-Process -FilePath $Exe -ArgumentList $DemoArguments -PassThru
} else {
    $process = Start-Process -FilePath $Exe -PassThru
}

for ($attempt = 0; $attempt -lt 60; $attempt++) {
    Start-Sleep -Milliseconds 150
    $process.Refresh()
    if ($process.HasExited) { throw 'demo exited before opening a window' }
    if ($process.MainWindowHandle -ne [IntPtr]::Zero) { break }
}

$handle = $process.MainWindowHandle
if ($handle -eq [IntPtr]::Zero) { $process.Kill(); throw 'demo window never appeared' }

# Always on top, otherwise another window can cover it and the capture shows that instead.
# SW_RESTORE first: a minimised window reports -32000 as its position and no client area,
# which used to produce a screenshot of the desktop with a sidecar that lied about it.
[void][Win32Capture]::ShowWindow($handle, 9)
[void][Win32Capture]::SetWindowPos($handle, [IntPtr](-1), 0, 0, 0, 0, 0x0043)
[void][Win32Capture]::SetForegroundWindow($handle)
Start-Sleep -Milliseconds $DelayMs

# Wait for a client area that is really there, then read the geometry in the same
# breath as the pixels, so the sidecar cannot describe a different moment.
$clientRect = New-Object Win32Capture+RECT
$usable = $false

for ($attempt = 0; $attempt -lt 40; $attempt++) {

    $process.Refresh()
    if ($process.HasExited) { throw 'demo exited before presenting a client area' }

    [void][Win32Capture]::GetClientRect($handle, [ref]$clientRect)

    if (($clientRect.Right - $clientRect.Left) -gt 0 -and ($clientRect.Bottom - $clientRect.Top) -gt 0) {
        $usable = $true
        break
    }

    [void][Win32Capture]::ShowWindow($handle, 9)
    Start-Sleep -Milliseconds 150
}

if (-not $usable) { $process.Kill(); throw 'demo never presented a usable client area' }

$rect = New-Object Win32Capture+RECT
[void][Win32Capture]::GetWindowRect($handle, [ref]$rect)
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
Write-Host "[capture] window $width x $height at $($rect.Left),$($rect.Top)"

function Grab([int]$left, [int]$top, [int]$w, [int]$h) {
    $bitmap = New-Object System.Drawing.Bitmap $w, $h
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.CopyFromScreen($left, $top, 0, 0, $bitmap.Size)
    $graphics.Dispose()
    return $bitmap
}

function GrabHash([System.Drawing.Bitmap]$bitmap) {
    $stream = New-Object System.IO.MemoryStream
    $bitmap.Save($stream, [System.Drawing.Imaging.ImageFormat]::Bmp)
    $bytes = $stream.ToArray()
    $stream.Dispose()

    $sha = [System.Security.Cryptography.SHA256]::Create()
    $hash = [System.BitConverter]::ToString($sha.ComputeHash($bytes))
    $sha.Dispose()

    return $hash
}

$bitmap = $null
$previous = $null

for ($attempt = 0; $attempt -lt 5; $attempt++) {
    $current = Grab $rect.Left $rect.Top $width $height
    $currentHash = GrabHash $current

    if ($null -ne $previous -and $previous -eq $currentHash) {
        $bitmap = $current
        break
    }

    Start-Sleep -Milliseconds 200
    $previous = $currentHash

    if ($null -ne $bitmap) { $bitmap.Dispose() }
    $bitmap = $current
}

New-Item -ItemType Directory -Force -Path (Split-Path $Out) | Out-Null
$bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()

# Where the client area sits inside the screenshot (title bar and border
# offsets), which the pixel tests need. Without it a test would have to guess
# the border width, and a theme or DPI change would break it.
[void][Win32Capture]::GetClientRect($handle, [ref]$clientRect)
$clientOrigin = New-Object Win32Capture+POINT
[void][Win32Capture]::ClientToScreen($handle, [ref]$clientOrigin)

$sidecar = [ordered]@{
    clientX      = $clientOrigin.X - $rect.Left
    clientY      = $clientOrigin.Y - $rect.Top
    clientWidth  = $clientRect.Right - $clientRect.Left
    clientHeight = $clientRect.Bottom - $clientRect.Top
    windowWidth  = $width
    windowHeight = $height
    arguments    = $DemoArguments
}

$sidecarPath = [System.IO.Path]::ChangeExtension($Out, '.json')
$sidecar | ConvertTo-Json | Set-Content -Path $sidecarPath -Encoding utf8
Write-Host "[capture] client area $($sidecar.clientWidth) x $($sidecar.clientHeight) at +$($sidecar.clientX)+$($sidecar.clientY) -> $sidecarPath"

Start-Sleep -Milliseconds 150

if (-not $process.HasExited) {

    $process.Kill()

    # Waiting for the exit is what makes the *next* capture safe: Kill returns
    # before the window is gone.
    [void]$process.WaitForExit(3000)

}

Write-Host "[capture] saved $Out ($((Get-Item $Out).Length) bytes)"


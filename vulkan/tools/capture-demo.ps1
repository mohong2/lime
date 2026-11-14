# Starts the demo, takes one screenshot, then closes it.
# Purpose: proof that Vulkan really draws (for CI and for a hand-over).
#
#   pwsh -File tools/capture-demo.ps1
#   pwsh -File tools/capture-demo.ps1 -DemoArguments '--scene=batch --quads=10000' -Out logs/screenshots/batch.png
param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\bin\lime-vulkan-demo.exe'),
    [string]$Out = (Join-Path $PSScriptRoot '..\logs\screenshots\demo.png'),
    [string]$DemoArguments = '',
    [int]$DelayMs = 1500
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class Win32Capture {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hWnd, ref POINT lpPoint);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int X, Y; }
}
'@

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

# Always on top, otherwise another window can cover it and the capture shows that instead
[void][Win32Capture]::SetWindowPos($handle, [IntPtr](-1), 0, 0, 0, 0, 0x0043)
[void][Win32Capture]::SetForegroundWindow($handle)
Start-Sleep -Milliseconds $DelayMs

$rect = New-Object Win32Capture+RECT
[void][Win32Capture]::GetWindowRect($handle, [ref]$rect)
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
Write-Host "[capture] window $width x $height at $($rect.Left),$($rect.Top)"

$bitmap = New-Object System.Drawing.Bitmap $width, $height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)

New-Item -ItemType Directory -Force -Path (Split-Path $Out) | Out-Null
$bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$graphics.Dispose()
$bitmap.Dispose()

# Where the client area sits inside the screenshot (title bar and border
# offsets), which the pixel tests need. Without it a test would have to guess
# the border width, and a theme or DPI change would break it.
$clientRect = New-Object Win32Capture+RECT
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
if (-not $process.HasExited) { $process.Kill() }

Write-Host "[capture] saved $Out ($((Get-Item $Out).Length) bytes)"

# Runs lime-vulkan-attach on the window of a running game and captures the evidence.
#
#   pwsh -File tools/capture-attach.ps1 -TargetPid 1234 -Seconds 12
#
# Writes logs/screenshots/attach-game.png: the game window shows the Vulkan
# gradient and the title bar is temporarily rewritten to
# '... [lime-vulkan N fps]' by lime-vulkan-attach.
# Note: the parameter cannot be called -Pid, since $Pid is a read-only
# automatic variable in PowerShell.
param(
    [Parameter(Mandatory = $true)][int]$TargetPid,
    [string]$Out = (Join-Path $PSScriptRoot '..\logs\screenshots\attach-game.png'),
    [int]$Seconds = 14,
    [int]$WarmupMs = 2500,
    [string]$AttachExe = (Join-Path $PSScriptRoot '..\bin\lime-vulkan-attach.exe'),
    [string]$LogPath = (Join-Path $PSScriptRoot '..\build\attach.log')
)

$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class AttachCapture {
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hWnd, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
}
'@

$target = Get-Process -Id $TargetPid -ErrorAction Stop
$target.Refresh()
$handle = $target.MainWindowHandle
if ($handle -eq [IntPtr]::Zero) { throw "pid $TargetPid has no main window" }

New-Item -ItemType Directory -Force -Path (Split-Path $LogPath) | Out-Null

Write-Host "[capture] attaching to pid $TargetPid, window $handle"
$arguments = @("--pid=$TargetPid", "--seconds=$Seconds", "--pattern=gradient")
$attach = Start-Process -FilePath $AttachExe -ArgumentList $arguments -PassThru -NoNewWindow -RedirectStandardOutput $LogPath -RedirectStandardError "$LogPath.err"

Start-Sleep -Milliseconds $WarmupMs

# SWP_NOSIZE|SWP_NOMOVE|SWP_SHOWWINDOW(0x0040)|SWP_NOACTIVATE
[void][AttachCapture]::SetWindowPos($handle, [IntPtr](-1), 0, 0, 0, 0, 0x0043)
[void][AttachCapture]::SetForegroundWindow($handle)
Start-Sleep -Milliseconds 900

$rect = New-Object AttachCapture+RECT
[void][AttachCapture]::GetWindowRect($handle, [ref]$rect)
$width = $rect.Right - $rect.Left
$height = $rect.Bottom - $rect.Top
Write-Host "[capture] window rect ${width}x${height} at $($rect.Left),$($rect.Top)"

$bitmap = New-Object System.Drawing.Bitmap $width, $height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)

New-Item -ItemType Directory -Force -Path (Split-Path $Out) | Out-Null
$bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$graphics.Dispose()
$bitmap.Dispose()
Write-Host "[capture] saved $Out ($((Get-Item $Out).Length) bytes)"

if (-not $attach.HasExited) { $attach.WaitForExit(($Seconds + 20) * 1000) | Out-Null }

Write-Host '--- lime-vulkan-attach output ---'
if (Test-Path $LogPath) { Get-Content $LogPath | Write-Host }
if (Test-Path "$LogPath.err") { Get-Content "$LogPath.err" | Write-Host }

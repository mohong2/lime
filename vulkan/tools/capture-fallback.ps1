param([string]$Exe = (Join-Path $PSScriptRoot '..\bin\lime-vulkan-demo.exe'),
      [string]$Out = (Join-Path $PSScriptRoot '..\logs\screenshots\fallback.png'),
      [string]$ArgsLine = '--device=99')

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.Windows.Forms

$process = Start-Process -FilePath $Exe -ArgumentList $ArgsLine -PassThru
Start-Sleep -Milliseconds 1800

$bounds = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
$bitmap = New-Object System.Drawing.Bitmap $bounds.Width, $bounds.Height
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.CopyFromScreen($bounds.X, $bounds.Y, 0, 0, $bitmap.Size)
New-Item -ItemType Directory -Force -Path (Split-Path $Out) | Out-Null
$bitmap.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
$graphics.Dispose(); $bitmap.Dispose()

if (-not $process.HasExited) { $process.Kill() }
Write-Host "[capture] $Out"
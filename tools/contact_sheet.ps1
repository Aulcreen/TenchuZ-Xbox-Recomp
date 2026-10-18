# Combine run screenshots into one labeled grid image.
# Usage: powershell -File tools/contact_sheet.ps1 -Name boot_10 [-Cols 4] [-Width 480]
# Reads logs/<Name>_<t>s.png, writes logs/<Name>_sheet.png
param([string]$Name, [int]$Cols = 4, [int]$Width = 480)
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent $PSScriptRoot
$files = Get-ChildItem -LiteralPath (Join-Path $root "logs") -Filter "${Name}_*s.png" |
  Sort-Object { [int]($_.BaseName -replace '.*_(\d+)s$', '$1') }
if (-not $files) { "no screenshots for $Name"; exit 1 }
$height = [int]($Width * 9 / 16)
$rows = [math]::Ceiling($files.Count / $Cols)
$sheet = New-Object System.Drawing.Bitmap ($Width * $Cols), ($height * $rows)
$g = [System.Drawing.Graphics]::FromImage($sheet)
$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
$font = New-Object System.Drawing.Font "Segoe UI", 14, ([System.Drawing.FontStyle]::Bold)
for ($i = 0; $i -lt $files.Count; $i++) {
  $img = [System.Drawing.Image]::FromFile($files[$i].FullName)
  $x = ($i % $Cols) * $Width; $y = [math]::Floor($i / $Cols) * $height
  $g.DrawImage($img, $x, $y, $Width, $height)
  $label = $files[$i].BaseName -replace '.*_(\d+s)$', '$1'
  $g.FillRectangle([System.Drawing.Brushes]::Black, $x, $y, 64, 26)
  $g.DrawString($label, $font, [System.Drawing.Brushes]::Yellow, ($x + 4), ($y + 2))
  $img.Dispose()
}
$out = Join-Path $root "logs\${Name}_sheet.png"
$sheet.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
$g.Dispose(); $sheet.Dispose()
$out

# Boot tenchuz.exe, take window screenshots at given times, then stop it.
# Usage: powershell -File tools/run_capture.ps1 -Name boot_06 -Seconds 60 -Shots 20,40,58 [-LogLevel debug]
# Outputs logs/<Name>.log and logs/<Name>_<t>s.png
param(
  [string]$Name = "boot",
  [int]$Seconds = 60,
  [int[]]$Shots = @(55),
  [string]$LogLevel = "info",
  [string[]]$Extra = @()
)
$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $root "out\build\win-amd64-relwithdebinfo\tenchuz.exe"
$log  = Join-Path $root "logs\$Name.log"
Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue

Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
"@
[W]::SetProcessDPIAware() | Out-Null

$args = @("--game_data_root=`"$root\game`"", "--log_file=`"$log`"", "--log_level=$LogLevel") + $Extra
$p = Start-Process -FilePath $exe -ArgumentList $args -WorkingDirectory $root -PassThru
$start = Get-Date
foreach ($t in ($Shots | Sort-Object)) {
  while (((Get-Date) - $start).TotalSeconds -lt $t) {
    if ($p.HasExited) { break }
    Start-Sleep -Milliseconds 250
  }
  if ($p.HasExited) { break }
  $p.Refresh()
  $h = $p.MainWindowHandle
  $r = New-Object W+RECT
  if ($h -ne [IntPtr]::Zero -and [W]::GetWindowRect($h, [ref]$r) -and ($r.R - $r.L) -gt 0) {
    $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, $bmp.Size)
    $png = Join-Path $root "logs\${Name}_${t}s.png"
    $bmp.Save($png, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
    "shot ${t}s -> $png"
  } else { "shot ${t}s: no window" }
}
while (-not $p.HasExited -and ((Get-Date) - $start).TotalSeconds -lt $Seconds) { Start-Sleep -Milliseconds 250 }
if ($p.HasExited) { "exited with code $($p.ExitCode) after $([int]((Get-Date)-$start).TotalSeconds)s" }
else { $p.Refresh(); "alive after ${Seconds}s, CPU $([math]::Round($p.CPU,1))s, RAM $([math]::Round($p.WorkingSet64/1MB)) MB"; Stop-Process -Id $p.Id -Force }

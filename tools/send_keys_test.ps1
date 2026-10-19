# Launch tenchuz.exe, send key presses to its window at given times, then stop it.
# Usage: powershell -File tools/send_keys_test.ps1 -Name keytest -Seconds 40 -Keys "20:{F10}","30:{F10}" [-Extra ...]
# Keys use WScript SendKeys syntax; SHOT takes a window screenshot, BURST 12 quick ones,
# HOLDxx holds virtual key 0xxx for 150 ms (e.g. HOLD0D = Enter).
# Output: logs/<Name>.log, logs/<Name>_<t>s.png
param(
  [string]$Name = "keytest",
  [int]$Seconds = 40,
  [string[]]$Keys = @(),
  [string[]]$Extra = @()
)
$root = Split-Path -Parent $PSScriptRoot
$exe  = Join-Path $root "out\build\win-amd64-relwithdebinfo\tenchuz.exe"
$log  = Join-Path $root "logs\$Name.log"
Remove-Item -LiteralPath $log -ErrorAction SilentlyContinue
$args = @("--game_data_root=`"$root\game`"", "--log_file=`"$log`"", "--log_level=info") + $Extra
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class W {
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
}
"@
[W]::SetProcessDPIAware() | Out-Null
$p = Start-Process -FilePath $exe -ArgumentList $args -WorkingDirectory $root -PassThru
$shell = New-Object -ComObject WScript.Shell
$start = Get-Date
foreach ($k in $Keys) {
  $t, $key = $k.Split(":", 2)
  while (((Get-Date) - $start).TotalSeconds -lt [int]$t -and -not $p.HasExited) { Start-Sleep -Milliseconds 200 }
  if ($p.HasExited) { break }
  if ($key -eq "SHOT" -or $key -eq "BURST") {
    # BURST: 12 half-size shots ~50 ms apart, to see frame-to-frame changes.
    $count = if ($key -eq "BURST") { 12 } else { 1 }
    $p.Refresh()
    $r = New-Object W+RECT
    if ([W]::GetWindowRect(($h = $p.MainWindowHandle), [ref]$r) -and ($r.R - $r.L) -gt 0) {
      for ($i = 0; $i -lt $count; ++$i) {
        $bmp = New-Object System.Drawing.Bitmap ($r.R - $r.L), ($r.B - $r.T)
        $g = [System.Drawing.Graphics]::FromImage($bmp)
        $hdc = $g.GetHdc(); [void][W]::PrintWindow($h, $hdc, 2); $g.ReleaseHdc($hdc)
        if ($count -eq 1) {
          $out = Join-Path $root "logs\${Name}_${t}s.png"
          $bmp.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
        } else {
          $small = New-Object System.Drawing.Bitmap $bmp, ([int](($r.R - $r.L) / 2)), ([int](($r.B - $r.T) / 2))
          $out = Join-Path $root "logs\${Name}_${t}s_$i.jpg"
          $small.Save($out, [System.Drawing.Imaging.ImageFormat]::Jpeg)
          $small.Dispose()
          Start-Sleep -Milliseconds 50
        }
        $g.Dispose(); $bmp.Dispose()
      }
      "shot ${t}s x$count"
    }
    continue
  }
  [void]$shell.AppActivate($p.Id)
  Start-Sleep -Milliseconds 300
  if ($key -match '^HOLD([0-9A-Fa-f]{2})$') {
    # HOLDxx: hold virtual key 0xxx for 150 ms (games poll input once a frame).
    $vk = [Convert]::ToByte($Matches[1], 16)
    [W]::keybd_event($vk, 0, 0, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds 150
    [W]::keybd_event($vk, 0, 2, [UIntPtr]::Zero)
    "held $key at ${t}s"
    continue
  }
  $shell.SendKeys($key)
  "sent $key at ${t}s"
}
while (-not $p.HasExited -and ((Get-Date) - $start).TotalSeconds -lt $Seconds) { Start-Sleep -Milliseconds 250 }
if (-not $p.HasExited) { Stop-Process -Id $p.Id -Force }

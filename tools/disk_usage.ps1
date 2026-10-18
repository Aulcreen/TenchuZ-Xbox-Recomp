# Report logical size vs. actual size on disk (after NTFS/WOF compression).
# Usage: powershell -File tools/disk_usage.ps1 <path> [<path> ...]
param([Parameter(ValueFromRemainingArguments = $true)][string[]]$Paths)
Add-Type @"
using System; using System.Runtime.InteropServices;
public static class D {
  [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
  public static extern uint GetCompressedFileSizeW(string name, out uint high);
}
"@
function OnDisk([string]$f) {
  $hi = 0; $lo = [D]::GetCompressedFileSizeW($f, [ref]$hi)
  return ([uint64]$hi -shl 32) + $lo
}
foreach ($p in $Paths) {
  $files = if (Test-Path -LiteralPath $p -PathType Leaf) { Get-Item -LiteralPath $p } else { Get-ChildItem -LiteralPath $p -Recurse -File -Force -ErrorAction SilentlyContinue }
  $logical = 0; $disk = 0
  foreach ($f in $files) { $logical += $f.Length; $disk += OnDisk $f.FullName }
  "{0,-40} {1,9:N0} MB logical  {2,9:N0} MB on disk  ({3:P0})" -f $p, ($logical/1MB), ($disk/1MB), ($(if ($logical) { $disk/$logical } else { 1 }))
}

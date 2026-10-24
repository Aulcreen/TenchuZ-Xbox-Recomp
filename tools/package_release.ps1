# Package a test build for other PCs: one installer, dist\TenchuZ-Setup-<version>.exe.
#   powershell -File tools/package_release.ps1 [-Preset win-amd64-relwithdebinfo] [-NoPdb]
# Stages the exe, its DLLs, the VC++ runtime (app-local), Setup.exe and the README
# in dist\TenchuZ-Recomp-<version>\, then appends them to Setup.exe (--pack).
# Never game data: the installer asks for the player's own disc image.
param(
  [string]$Preset = "win-amd64-relwithdebinfo",
  [switch]$NoPdb
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root "out\build\$Preset"
$version = (git -C $root describe --always --dirty 2>$null)
if (-not $version) { $version = Get-Date -Format "yyyyMMdd-HHmm" }
$name = "TenchuZ-Recomp-$version"
$dist = Join-Path $root "dist"
$stage = Join-Path $dist $name

foreach ($required in @("tenchuz.exe", "Setup.exe")) {
  if (-not (Test-Path (Join-Path $build $required))) { throw "$required not built in $build (run build.bat)" }
}
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force $stage | Out-Null

# Setup.exe goes in too: run from the install folder, it re-copies the disc image.
$files = @("tenchuz.exe", "Setup.exe") + (Get-ChildItem $build -Filter "*.dll" | ForEach-Object { $_.Name })
if (-not $NoPdb -and (Test-Path (Join-Path $build "tenchuz.pdb"))) { $files += "tenchuz.pdb" }  # symbolized crash reports
foreach ($f in $files) { Copy-Item (Join-Path $build $f) $stage }
# Steam Input's action manifest and controller configurations (src/input/steam_input.h).
$steamInput = Join-Path $build "steam_input"
if (Test-Path $steamInput) { Copy-Item -Recurse $steamInput $stage }

# VC++ 2022 runtime, app-local (redistributable), from the build tools' redist folder.
$redist = Get-ChildItem "C:\Program Files*\Microsoft Visual Studio\2022\*\VC\Redist\MSVC\*\x64\Microsoft.VC143.CRT" -Directory -ErrorAction SilentlyContinue |
  Sort-Object FullName | Select-Object -Last 1
if (-not $redist) { throw "VC++ redist folder (Microsoft.VC143.CRT) not found" }
foreach ($dll in @("msvcp140.dll", "msvcp140_atomic_wait.dll", "vcruntime140.dll", "vcruntime140_1.dll")) {
  Copy-Item (Join-Path $redist.FullName $dll) $stage
}

Copy-Item (Join-Path $PSScriptRoot "release\README.txt") $stage

$installer = Join-Path $dist "TenchuZ-Setup-$version.exe"
if (Test-Path $installer) { Remove-Item -Force $installer }
& (Join-Path $build "Setup.exe") --pack $stage $installer
if ($LASTEXITCODE -ne 0) { throw "Setup.exe --pack failed ($LASTEXITCODE)" }
$size = [math]::Round((Get-Item $installer).Length / 1MB, 1)
"Packaged $name ($($files.Count + 5) files) -> $installer ($size MB)"

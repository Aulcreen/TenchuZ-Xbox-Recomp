@echo off
rem Build tenchuz. Usage: build.bat [preset]   (default: win-amd64-relwithdebinfo)
rem Requires VS 2022 Build Tools (C++ + Clang). Downloads the ReXGlue SDK to third_party\rexglue-bin.
setlocal
set PRESET=%1
if "%PRESET%"=="" set PRESET=win-amd64-relwithdebinfo

set VSROOT=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools
call "%VSROOT%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
rem Prefer a standalone LLVM (Clang 20+) if installed, else VS's bundled clang.
if exist "C:\Program Files\LLVM\bin\clang++.exe" (
  set "PATH=C:\Program Files\LLVM\bin;%PATH%"
) else (
  set "PATH=%VSROOT%\VC\Tools\Llvm\x64\bin;%PATH%"
)

cd /d "%~dp0"
rem The ReXGlue SDK build this project uses (tools\macos\build.sh downloads the same one).
set SDK_TAG=nightly-20261002-bd833a2a
set SDK_ZIP=rexglue-sdk-0.10.0.24-dev.gbd833a2-win-amd64.zip
if not exist "third_party\rexglue-bin\win-amd64\bin\rexglue.exe" (
  echo Downloading the ReXGlue SDK %SDK_TAG%
  if not exist "third_party\rexglue-bin" mkdir "third_party\rexglue-bin"
  curl.exe -fL -o "third_party\rexglue-bin\%SDK_ZIP%" "https://github.com/rexglue/rexglue-sdk/releases/download/%SDK_TAG%/%SDK_ZIP%" || exit /b 1
  tar.exe -xf "third_party\rexglue-bin\%SDK_ZIP%" -C "third_party\rexglue-bin" || exit /b 1
  del "third_party\rexglue-bin\%SDK_ZIP%"
)
rem The first configure needs codegen's source list (later builds re-run codegen themselves).
if not exist "generated\default\sources.cmake" (
  "third_party\rexglue-bin\win-amd64\bin\rexglue.exe" codegen tenchuz_manifest.toml || exit /b 1
)
cmake --preset %PRESET% -DCMAKE_PREFIX_PATH="%~dp0third_party\rexglue-bin\win-amd64" || exit /b 1
cmake --build --preset %PRESET% || exit /b 1

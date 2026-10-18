@echo off
rem Build tenchuz. Usage: build.bat [preset]   (default: win-amd64-relwithdebinfo)
rem Requires VS 2022 Build Tools (C++ + Clang) and the ReXGlue SDK in third_party\rexglue-bin.
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
cmake --preset %PRESET% -DCMAKE_PREFIX_PATH="%~dp0third_party\rexglue-bin\win-amd64" || exit /b 1
cmake --build --preset %PRESET% || exit /b 1

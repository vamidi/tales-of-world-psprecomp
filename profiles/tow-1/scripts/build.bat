@echo off
setlocal EnableExtensions
for %%I in ("%~dp0..\..\..") do set "REPO=%%~fI"
set "BUILD=%REPO%\out\tow-1"

set "VSDEV=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat"
if not exist "%VSDEV%" (
    echo ERROR: VsDevCmd.bat not found at:
    echo   %VSDEV%
    echo Adjust VSDEV in this script for your Visual Studio installation.
    exit /b 1
)
call "%VSDEV%" -arch=x64 -host_arch=x64 -no_logo >nul
if errorlevel 1 exit /b 1

set "JOBS=%PSPRECOMP_JOBS%"
if not defined JOBS set "JOBS=8"

echo Configuring tow-1 (profile=tow-1, AOT /MP%JOBS%, /Ob0)...
cmake -S "%REPO%" -B "%BUILD%" -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DPSPRECOMP_PROFILE=tow-1 ^
  -DPSPRECOMP_MSVC_MP_JOBS=%JOBS% ^
  -DPSPRECOMP_GENERATED_OPT_LEVEL=2 ^
  -DPSPRECOMP_BUILD_PROFILE_TESTS=OFF
if errorlevel 1 exit /b 1

echo Building Tow1Native...
cmake --build "%BUILD%" --target Tow1Native
if errorlevel 1 exit /b 1

echo BUILD OK: %BUILD%\bin\Tow1Native.exe
exit /b 0

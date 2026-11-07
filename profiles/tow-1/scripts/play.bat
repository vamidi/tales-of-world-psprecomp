@echo off
setlocal EnableExtensions
for %%I in ("%~dp0..\..\..") do set "REPO=%%~fI"
set "EXE=%REPO%\out\tow-1\bin\Tow1Native.exe"
if not exist "%EXE%" (
    echo ERROR: %EXE% not found. Run scripts\build.bat first.
    exit /b 1
)
"%EXE%" "%REPO%\profiles\tow-1\game\eboot.dec" "%REPO%\profiles\tow-1\game\root" %*
exit /b %ERRORLEVEL%

@echo off
setlocal EnableExtensions
for %%I in ("%~dp0..\..\..") do set "REPO=%%~fI"
set "EXE=%REPO%\out\tow-2\bin\Tow2Native.exe"
if not exist "%EXE%" (
    echo ERROR: %EXE% not found. Run scripts\build.bat first.
    exit /b 1
)
"%EXE%" "%REPO%\profiles\tow-2\game\eboot.dec" "%REPO%\profiles\tow-2\game\root" %*
exit /b %ERRORLEVEL%

@echo off
setlocal EnableExtensions
for %%I in ("%~dp0..\..\..") do set "REPO=%%~fI"
set "RECOMP=%REPO%\out\framework\psp_recomp.exe"
set "EBOOT=%REPO%\profiles\tow-2\game\eboot.dec"
set "OUT=%REPO%\profiles\tow-2\generated"

if not exist "%RECOMP%" (
    echo ERROR: %RECOMP% not found. Build the framework tools first:
    echo   call "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64
    echo   cmake -S "%REPO%" -B "%REPO%\out\framework" -G Ninja -DPSPRECOMP_PROFILE=""
    echo   cmake --build "%REPO%\out\framework" --target psp_recomp
    exit /b 1
)
if not exist "%EBOOT%" (
    echo ERROR: %EBOOT% not found.
    echo Place the decrypted Tales of the World: Radiant Mythology 2 EBOOT there.
    exit /b 1
)

echo Generating tow-2 corpus from %EBOOT%
"%RECOMP%" "%EBOOT%" --auto "%OUT%" 0x08804000 0x2000
exit /b %ERRORLEVEL%

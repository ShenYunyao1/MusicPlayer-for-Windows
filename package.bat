@echo off
setlocal
set "package_exit=1"
pushd "%~dp0"
if errorlevel 1 goto finish
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0package.ps1" %*
set "package_exit=%errorlevel%"
popd
:finish
echo.
if not "%package_exit%"=="0" echo PACKAGE FAILED: exit code %package_exit%. Review the error above.
if /i not "%MUSIC_PLAYER_NO_PAUSE%"=="1" (
    echo Press any key to close this window...
    pause >nul
)
exit /b %package_exit%

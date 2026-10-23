@echo off
setlocal
set "publish_exit=1"
pushd "%~dp0"
if errorlevel 1 goto finish
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0publish.ps1" %*
set "publish_exit=%errorlevel%"
popd
:finish
echo.
if not "%publish_exit%"=="0" echo PUBLISH FAILED: exit code %publish_exit%. Review the error above.
if /i not "%MUSIC_PLAYER_NO_PAUSE%"=="1" (
    echo Press any key to close this window...
    pause >nul
)
exit /b %publish_exit%

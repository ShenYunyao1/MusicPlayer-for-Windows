@echo off
setlocal

set "compile_output=%~1"
if not defined compile_output set "compile_output=%~dp0..\build\obj\MusicPlayer2\compile_time.txt"
for %%F in ("%compile_output%") do if not exist "%%~dpF" mkdir "%%~dpF"
if errorlevel 1 exit /b 1

:: Get formated date time by PowerShell.
for /f "tokens=*" %%i in ('powershell -Command "Get-Date -Format 'yyyy-MM-dd HH:mm:ss'"') do set "formatted_datetime=%%i"

:: output to ompile_time.txt file.
echo %formatted_datetime% > "%compile_output%"

:: Get the current Git commit hash (without showing error messages)
for /f %%i in ('git -C "%~dp0.." rev-parse HEAD 2^>nul') do set commit_hash=%%i

:: If the git command is executed successfully, the commit hash is appended to the compile_time.txt file.
if not "%commit_hash%"=="" (
    echo %commit_hash% >> "%compile_output%"
)

endlocal

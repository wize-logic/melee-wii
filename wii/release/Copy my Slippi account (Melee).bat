@echo off
rem Melee for the Wii: copies your Slippi account (from Slippi Launcher on this
rem PC) to the Wii's SD card or USB drive. Double-click it there, after
rem extracting the release zip onto the top of that drive.
title Melee for the Wii: copy your Slippi account
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%~dp0apps\melee\slippi\copy-account.ps1" (
    echo This file belongs on the Wii's SD card or USB drive, next to the apps folder.
    echo Extract the whole release zip onto the top of that drive, then run it from there.
    echo.
    pause
    exit /b 1
)
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0apps\melee\slippi\copy-account.ps1" -Here "%~dp0."
echo.
pause

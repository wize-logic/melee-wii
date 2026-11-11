@echo off
rem Melee for the Wii: copies your Slippi account (from Slippi Launcher on this
rem PC) to the SD card. Double-click it on the SD card, after extracting the
rem release zip there.
title Melee for the Wii: copy your Slippi account
set "PS=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%~dp0apps\melee\slippi\copy-account.ps1" (
    echo This file belongs on the SD card, next to the apps folder.
    echo Extract the whole release zip onto your SD card, then run it from there.
    echo.
    pause
    exit /b 1
)
"%PS%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0apps\melee\slippi\copy-account.ps1" -Here "%~dp0."
echo.
pause

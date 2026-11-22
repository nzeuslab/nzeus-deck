@echo off
rem Puts the console's own system back on an R36S SD card. Windows will ask once for administrator rights.
set "S=%~dp0more\installer"
if not exist "%S%" set "S=%~dp0installer"
powershell -NoProfile -ExecutionPolicy Bypass -File "%S%\uninstall.ps1"

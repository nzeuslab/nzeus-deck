@echo off
rem Installs NZEUS Deck on an R36S SD card. Windows will ask once for administrator rights.
set "S=%~dp0more\installer"
if not exist "%S%" set "S=%~dp0installer"
powershell -NoProfile -ExecutionPolicy Bypass -File "%S%\install.ps1"

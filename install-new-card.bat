@echo off
rem Installs NZEUS Deck on a NEW, empty SD card (your original card is only read). Windows will ask once for administrator rights.
set "S=%~dp0more\installer"
if not exist "%S%" set "S=%~dp0installer"
powershell -NoProfile -ExecutionPolicy Bypass -File "%S%\new-card.ps1"

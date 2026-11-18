@echo off
rem Installs NZEUS Deck on an R36S SD card. Windows will ask once for administrator rights.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\install.ps1"

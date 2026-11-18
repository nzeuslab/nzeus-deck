@echo off
rem Puts the console's own system back on an R36S SD card. Windows will ask once for administrator rights.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\uninstall.ps1"

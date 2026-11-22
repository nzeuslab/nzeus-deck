@echo off
rem Copies NZEUS's log files off the R36S SD card for a bug report. Windows will ask once for administrator rights.
set "S=%~dp0more\installer"
if not exist "%S%" set "S=%~dp0installer"
powershell -NoProfile -ExecutionPolicy Bypass -File "%S%\collect-logs.ps1"

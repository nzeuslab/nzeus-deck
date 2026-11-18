@echo off
rem Copies NZEUS's log files off the R36S SD card for a bug report. Windows will ask once for administrator rights.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\collect-logs.ps1"

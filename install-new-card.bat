@echo off
rem Installs NZEUS Deck on a NEW, empty SD card (your original card is only read). Windows will ask once for administrator rights.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\new-card.ps1"

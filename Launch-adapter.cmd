@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0launch-adapter.ps1"
if errorlevel 1 pause

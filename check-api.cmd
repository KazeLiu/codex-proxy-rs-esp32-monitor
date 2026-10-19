@echo off
chcp 65001 >nul
title CPR Online API Demo
powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0check-api.ps1"
echo.
pause

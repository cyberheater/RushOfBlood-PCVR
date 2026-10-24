@echo off
rem Builds RushVR with the Move-controllers patch into this folder. See build-rushvr.ps1.
title Building RushVR
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-rushvr.ps1"
echo.
pause

@echo off
title Super Smash Bros. Brawl recomp build
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build-brawl.ps1" %*
echo.
echo Finished with exit code %ERRORLEVEL%.
pause

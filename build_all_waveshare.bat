@echo off
REM build_all_waveshare.bat — Build all apps for the Waveshare ESP32-P4-WIFI6-Touch-LCD-4.3
REM and generate the merged firmware.  Wrapper around build_waveshare.ps1.
REM Usage: build_all_waveshare.bat          (Rev3.x silicon, current retail boards)
REM        build_all_waveshare.bat -Rev1    (older Rev1.x silicon boards)

set "ROOT=%~dp0"
if "%ROOT:~-1%"=="\" set "ROOT=%ROOT:~0,-1%"
powershell -ExecutionPolicy Bypass -File "%ROOT%\build_waveshare.ps1" %*
if errorlevel 1 ( echo FAILED: Waveshare build & pause & exit /b 1 )
pause

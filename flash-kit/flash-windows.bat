@echo off
rem CQNU AI Badge - double-click to flash (Windows). Arguments are passed through, e.g. flash-windows.bat -Update
cd /d "%~dp0"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0flash-windows.ps1" %*
echo.
pause

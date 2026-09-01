@echo off
chcp 65001 >nul
cd /d "%~dp0"
echo ==============================================================
echo   VoiceTableAssist - One-click Deploy and Self-check
echo   Will: install VC++ runtime if missing, start service,
echo        run self-test, then wait for you to press Enter to stop.
echo ==============================================================
echo.
powershell -NoProfile -ExecutionPolicy Bypass -File ".\deploy-check.ps1" -Selftest
echo.
echo Script finished. Press any key to close this window.
pause >nul

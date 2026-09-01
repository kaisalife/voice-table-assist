@echo off
REM ============================================================================
REM  install.bat - One-click deploy entry for VoiceTableAssist
REM  Delegates entirely to deploy-check.bat /SELFTEST.
REM  No PowerShell.
REM ============================================================================
setlocal EnableExtensions
chcp 65001 >nul
cd /d "%~dp0"
echo ==============================================================
echo   VoiceTableAssist - One-click Deploy and Self-check
echo ==============================================================
echo   Steps:
echo     1. Install VC++ 2015-2022 x64 runtime if missing
echo     2. Verify sherpa DLL integrity, files, free port
echo     3. Start VoiceTableAssist, wait until /api/health returns ok
echo     4. Run HTTP multi-table self-test ^(import + switch + NER^)
echo     5. Keep service running - you can open the web test page
echo     6. Press ENTER or Ctrl+C - auto-kill service + cleanup data
echo ==============================================================
echo.
call "%~dp0deploy-check.bat" /SELFTEST
set "RC=%ERRORLEVEL%"
echo.
if "%RC%"=="0" ( echo Script finished successfully. ) else ( echo Script finished with exit code %RC%. )
echo Press any key to close this window.
pause >nul
endlocal & exit /b %RC%

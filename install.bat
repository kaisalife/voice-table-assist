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
echo     1. Allow firewall ports 15232/15433 for LAN access
echo     2. Install VC++ 2015-2022 x64 runtime if missing
echo     3. Verify sherpa DLL integrity, files, free port
echo     4. Start VoiceTableAssist, wait until /api/health returns ok
echo     5. Run HTTP multi-table self-test ^(import + switch + NER^)
echo     6. Keep service running - you can open the web test page
echo     7. Press ENTER or Ctrl+C - auto-kill service + cleanup data
echo ==============================================================
echo.
REM ---- 放行局域网访问端口（手机/平板直访 http://IP:15232 / https://IP:15433 必须）----
netsh advfirewall firewall show rule name="VoiceTableAssist" >NUL 2>&1
if errorlevel 1 (
    netsh advfirewall firewall add rule name="VoiceTableAssist" dir=in action=allow protocol=TCP localport=15232,15433 >NUL
    echo [OK] 已放行防火墙端口 15232/15433（手机/平板局域网直访）
) else (
    echo [OK] 防火墙规则 VoiceTableAssist 已存在
)
call "%~dp0deploy-check.bat" /SELFTEST
set "RC=%ERRORLEVEL%"
echo.
if "%RC%"=="0" ( echo Script finished successfully. ) else ( echo Script finished with exit code %RC%. )
echo Press any key to close this window.
pause >nul
endlocal & exit /b %RC%

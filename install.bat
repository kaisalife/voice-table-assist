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
REM ---- 放行局域网访问端口（手机/平板直访 http://IP:15232 必须）----
netsh advfirewall firewall show rule name="VoiceTableAssist" >NUL 2>&1
if errorlevel 1 (
    netsh advfirewall firewall add rule name="VoiceTableAssist" dir=in action=allow protocol=TCP localport=15232,15433 >NUL
    echo [OK] 已放行防火墙端口 15232/15433（平板/Cordova 壳局域网直访）
) else (
    echo [OK] 防火墙规则 VoiceTableAssist 已存在
)

REM ---- HTTPS 证书（可选，默认不随包）：平板走 Cordova 壳内的 http://localhost 安全上下文，不需要证书。
REM 仅当包内确实带了 make-cert.ps1 且证书缺失时才自愈；没带就跳过（HTTP 照常工作），不阻断部署。
set "NEEDS_CERT=0"
if not exist "%~dp0certs\gateway.pfx" set "NEEDS_CERT=1"
if "%NEEDS_CERT%"=="1" if exist "%~dp0make-cert.ps1" (
    echo.
    echo ==^> 检测到 make-cert.ps1（可选 HTTPS），证书缺失，自动生成中...
    where powershell >NUL 2>NUL
    if errorlevel 1 (
        echo WARN    未找到 powershell.exe，跳过 HTTPS 证书生成（仅 HTTP，不影响语音）
    ) else (
        powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0make-cert.ps1"
        if errorlevel 1 (
            echo WARN    make-cert.ps1 执行失败，跳过 HTTPS（仅 HTTP，不影响语音）
        ) else (
            echo [OK] HTTPS 证书已生成 certs\gateway.pfx + wwwroot\ca.crt
        )
    )
) else (
    echo [OK] 未启用 HTTPS（平板走 Cordova 壳，无需证书）
)

call "%~dp0deploy-check.bat" /SELFTEST
set "RC=%ERRORLEVEL%"
echo.
if "%RC%"=="0" ( echo Script finished successfully. ) else ( echo Script finished with exit code %RC%. )
echo Press any key to close this window.
pause >nul
endlocal & exit /b %RC%

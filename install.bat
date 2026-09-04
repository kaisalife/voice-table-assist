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
echo     3. Generate HTTPS certs if missing (certs\gateway.pfx + wwwroot\ca.crt)
echo     4. Verify sherpa DLL integrity, files, free port
echo     5. Start VoiceTableAssist, wait until /api/health returns ok
echo     6. Run HTTP multi-table self-test ^(import + switch + NER^)
echo     7. Keep service running - you can open the web test page
echo     8. Press ENTER or Ctrl+C - auto-kill service + cleanup data
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

REM ---- HTTPS 证书自愈：certs\gateway.pfx 或 wwwroot\ca.crt 缺失则自动跑 make-cert.ps1 ----
REM 平板浏览器走 https://<网关IP>:15433/ 需要 ca.crt；Kestrel 15433 监听需要 gateway.pfx。
REM 任一缺失就调 make-cert.ps1（脚本会把 ca.crt 同时复制到 wwwroot/，并保留根 CA 跨次运行复用）。
set "NEEDS_CERT=0"
if not exist "%~dp0certs\gateway.pfx" set "NEEDS_CERT=1"
if not exist "%~dp0wwwroot\ca.crt" set "NEEDS_CERT=1"
if "%NEEDS_CERT%"=="1" (
    echo.
    echo ==^> HTTPS 证书缺失，自动生成中（make-cert.ps1）...
    where powershell >NUL 2>NUL
    if errorlevel 1 (
        echo FAIL    系统找不到 powershell.exe，请先安装 PowerShell 后重试 install.bat
        echo         或手动在项目根目录执行: powershell -ExecutionPolicy Bypass -File make-cert.ps1
        echo Press any key to close this window.
        pause >nul
        endlocal & exit /b 1
    )
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0make-cert.ps1"
    set "RC=%ERRORLEVEL%"
    if not "%RC%"=="0" (
        echo FAIL    make-cert.ps1 退出码 %RC%。请检查上面的错误信息后重试。
        echo Press any key to close this window.
        pause >nul
        endlocal & exit /b %RC%
    )
    REM 防御性确认：脚本成功但文件确实没生成
    if not exist "%~dp0certs\gateway.pfx" (
        echo FAIL    make-cert.ps1 报告成功但 certs\gateway.pfx 不存在
        endlocal & exit /b 1
    )
    if not exist "%~dp0wwwroot\ca.crt" (
        echo FAIL    make-cert.ps1 报告成功但 wwwroot\ca.crt 不存在
        endlocal & exit /b 1
    )
    echo [OK] HTTPS 证书已生成 certs\gateway.pfx + wwwroot\ca.crt
) else (
    echo [OK] HTTPS 证书已就位 certs\gateway.pfx + wwwroot\ca.crt
)

call "%~dp0deploy-check.bat" /SELFTEST
set "RC=%ERRORLEVEL%"
echo.
if "%RC%"=="0" ( echo Script finished successfully. ) else ( echo Script finished with exit code %RC%. )
echo Press any key to close this window.
pause >nul
endlocal & exit /b %RC%

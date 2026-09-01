@echo off
REM ============================================================================
REM  make-cert.bat - one-click self-signed CA + gateway HTTPS certificate.
REM  Thin wrapper; real logic lives in make-cert.ps1 (ASCII-only output).
REM
REM  Usage (double-click, or):
REM    make-cert.bat                          auto-detect local IPv4 addresses
REM    make-cert.bat 192.168.1.10 10.0.0.5    add extra IPs into the SAN
REM
REM  Output: certs\ca.crt (install on tablets) + certs\gateway.pfx (Kestrel).
REM  After generating, restart VoiceTableAssist -> HTTPS on port 15433.
REM  Tablets: install ca.crt once, then open https://<gateway-ip>:15433/
REM ============================================================================
setlocal
REM Capture %~dp0 BEFORE any shift: after `shift`, %~dp0 resolves %0 wrongly.
set "PROJECT=%~dp0"
chcp 65001 >nul

where powershell >NUL 2>NUL
if errorlevel 1 echo FAIL  powershell not found. & goto :fail

powershell -NoProfile -ExecutionPolicy Bypass -File "%PROJECT%make-cert.ps1" %*
if errorlevel 1 goto :fail

echo.
echo [OK] HTTPS certificate ready. Restart the gateway to enable port 15433.
endlocal & exit /b 0

:fail
echo.
echo [FAIL] make-cert.bat stopped. See messages above.
endlocal & exit /b 1

@echo off
REM ============================================================================
REM  deploy-check.bat - Deployment smoke-checker (start temp, stop on exit)
REM  Pure batch (no PowerShell). Works on Chinese Windows:
REM  all control-flow text is ASCII. Only body temp JSON is UTF-8.
REM
REM  Usage (put this BAT in the same directory as VoiceTableAssist.exe):
REM    deploy-check.bat [/SELFTEST] [/PORT N] [/KEEPALIVE N] [/?]
REM      /SELFTEST         After HTTP ready, invoke selftest/selftest.bat
REM      /PORT N           HTTP health-check port (default 15232)
REM      /KEEPALIVE N      After ready, keep service alive N sec then exit
REM                        (CI / unattended mode; default 60s when SELFTEST)
REM ============================================================================
REM  Guarantee: on ANY exit path (normal / Ctrl+C / fail) the script first
REM  tries to kill VoiceTableAssist.exe + sherpa-onnx-online-websocket-server.exe
REM  started by this run (double taskkill /F + /T; SIG-file fence to avoid wiping
REM  stale processes that share the exe name). SELFTEST mode ALSO wipes
REM  aggregated tables/hotwords under models/embedding/tables and
REM  sherpa-onnx/hr/tables/* (keep current/hotwords.txt truncated).
REM ============================================================================
setlocal EnableExtensions EnableDelayedExpansion
REM Capture %~dp0 BEFORE the arg-parse loop: `shift` moves %0 as well, so
REM evaluating %~dp0 after parsing a switch would resolve to "D:\".
set "ROOT=%~dp0"
chcp 65001 >nul
cd /d "%ROOT%"

set "SELFTEST="
set "PORT=15232"
set "KEEPALIVE="
:parse
if "%~1"=="" goto :doneParse
if /I "%~1"=="/?"          goto :help
if /I "%~1"=="-?"          goto :help
if /I "%~1"=="--help"      goto :help
if /I "%~1"=="/SELFTEST"   set "SELFTEST=1"   & shift & goto :parse
if /I "%~1"=="-selftest"   set "SELFTEST=1"   & shift & goto :parse
if /I "%~1"=="/PORT"       set "PORT=%~2"     & shift & shift & goto :parse
if /I "%~1"=="-port"       set "PORT=%~2"     & shift & shift & goto :parse
if /I "%~1"=="/KEEPALIVE"  set "KEEPALIVE=%~2" & shift & shift & goto :parse
if /I "%~1"=="-keepalive"  set "KEEPALIVE=%~2" & shift & shift & goto :parse
REM Tolerate unknowns silently (e.g. accidental /VERBOSE leftover from ps1).
shift
goto :parse
:doneParse

if "%SELFTEST%"=="1" if "%KEEPALIVE%"=="" set "KEEPALIVE=60"
set "BASE=http://127.0.0.1:%PORT%"
set "EXE=%ROOT%VoiceTableAssist.exe"
set "SIG=%TEMP%\VTA-%RANDOM%-%RANDOM%-sig.txt"
set "PROC_RUN="
set "PID="

REM === Cleanup on every exit (Ctrl+C, goto :end, goto :fail, ERROR in sub) ===
REM     (cmd has no proper trap; we call :cleanup explicitly from every end
REM      branch; if the user Ctrl+C kills us the sig file gets orphaned but
REM      that is harmless.)

REM ===== 0) Require BAT next to VoiceTableAssist.exe =====
if not exist "%EXE%" (
    echo FAIL  VoiceTableAssist.exe missing in %ROOT%.
    echo        This BAT must sit next to it inside the deployment package.
    goto :fail
)

REM ===== 1) MOTW / SmartScreen reminder =====
echo ==^> SmartScreen / MOTW (cmd-only environment, no ADS delete primitive)...
echo NOTE:   If Windows blocks VoiceTableAssist.exe on first run, right-click
echo         the exe ^(or the zip before extracting^), open Properties, and
echo         check "Unblock". This BAT cannot automate that step from cmd.

REM ===== 2) sherpa-onnx native deps (--help launch + DLL list) =====
echo ==^> sherpa-onnx runtime deps ...
set "SHERPA_DIR="
if exist "%ROOT%sherpa-onnx\sherpa-onnx-online-websocket-server.exe" (
    set "SHERPA_DIR=%ROOT%sherpa-onnx"
) else if exist "%ROOT%models\sherpa-onnx\sherpa-onnx-online-websocket-server.exe" (
    set "SHERPA_DIR=%ROOT%models\sherpa-onnx"
)
if not defined SHERPA_DIR (
    echo FAIL    sherpa-onnx-online-websocket-server.exe not found.
    echo         Expected at %ROOT%sherpa-onnx\  or  %ROOT%models\sherpa-onnx\
    goto :fail
)
echo         SHERPA_DIR=!SHERPA_DIR!

echo ==^> Running sherpa server --help (exit must be 0)...
set "SHERPA_HELP_TMP=%TEMP%\sherpa-help-%RANDOM%.log"
"!SHERPA_DIR!\sherpa-onnx-online-websocket-server.exe" --help >"%SHERPA_HELP_TMP%" 2>&1
set "HELP_RC=!ERRORLEVEL!"
del "%SHERPA_HELP_TMP%" 2>NUL
if not "!HELP_RC!"=="0" (
    echo WARN    sherpa server --help exit !HELP_RC!; typical causes: missing VC++ / DX / dbghelp DLLs.
    echo         DLL status table:
    for %%D in (dxgi.dll msvcp140.dll vcruntime140.dll concrt140.dll
                dbghelp.dll SETUPAPI.dll WS2_32.dll MSWSOCK.dll) do (
        if exist "%SystemRoot%\System32\%%D" (echo         OK     %%D) else (echo         MISS   %%D)
    )
    echo FAIL    Please install latest VC++ 2015-2022 x64 redist and retry.
    goto :fail
)

echo ==^> 4 sherpa runtime DLLs next to server exe:
set "SHERPA_DLLS_MISS="
for %%D in (onnxruntime.dll
            onnxruntime_providers_shared.dll
            sherpa-onnx-c-api.dll
            sherpa-onnx-cxx-api.dll) do (
    if exist "!SHERPA_DIR!\%%D" (echo         OK     %%D) else (
        echo         MISS   %%D
        set "SHERPA_DLLS_MISS=!SHERPA_DLLS_MISS! %%D"
    )
)
if defined SHERPA_DLLS_MISS (
    echo FAIL    sherpa DLLs missing:!SHERPA_DLLS_MISS!.
    goto :fail
)

REM ===== 3) Deployment integrity =====
echo ==^> Deployment integrity ...
set "INTEG_MISS="
if exist "%ROOT%appsettings.json"   (echo         OK     appsettings.json) else (echo         MISS   appsettings.json & set "INTEG_MISS=1")
if exist "%ROOT%wwwroot\index.html" (echo         OK     wwwroot/index.html) else (echo         MISS   wwwroot/index.html & set "INTEG_MISS=1")
if exist "%ROOT%models\raner"       (echo         OK     models\raner\) else (echo         MISS   models\raner\ & set "INTEG_MISS=1")
if exist "%ROOT%models\embedding"   (echo         OK     models\embedding\) else (echo         MISS   models\embedding\ & set "INTEG_MISS=1")
if defined INTEG_MISS (
    echo FAIL    Core deployment missing; re-run publish.bat and re-extract.
    goto :fail
)

set "HOTWORDS=!SHERPA_DIR!\hr\tables\current\hotwords.txt"
if exist "!HOTWORDS!" (
    echo         OK     !HOTWORDS!
) else (
    echo WARN    !HOTWORDS! missing; create it as empty ^(this BAT will auto-create later^).
)

REM ===== 5) Port pre-check =====
echo ==^> Port pre-check %BASE%/api/health ...
where curl >NUL 2>NUL
if errorlevel 1 ( echo WARN    curl.exe not on PATH; skipping live port probe. ) else (
    curl -sS --max-time 2 "%BASE%/api/health" >NUL 2>NUL
    if not errorlevel 1 (
        echo FAIL    %BASE%/api/health is already answering. Stop the old instance first.
        goto :fail
    ) else ( echo         free )
)

REM ===== 5) Start service + readiness poll =====
echo ==^> Starting service: %EXE% ...
REM SIG file fence + write before start so we never kill stale processes.
echo %RANDOM% >"%SIG%"
start "VoiceTableAssist" /D "%ROOT%" /B "%EXE%"
REM `start` is a cmd builtin and does NOT reset ERRORLEVEL: the non-zero code
REM left by the port-probe curl above (refused by design when the port is
REM free) would look like a start failure. The health poll below is the gate.
set "PROC_RUN=1"

ping -n 2 127.0.0.1 >NUL
echo ==^> Capture latest PID via tasklist CSV ...
set "PID="
REM tasklist /FO CSV /NH rows look like: "VoiceTableAssist.exe","1234","Console","1","xx K"
REM tokens=2 (delims=comma) is the quoted PID; %%~P strips the quotes.
for /f "tokens=2 delims=," %%P in ('tasklist /FI "IMAGENAME eq VoiceTableAssist.exe" /FO CSV /NH 2^>NUL') do set "PID=%%~P"
if defined PID ( echo         PID=%PID% ) else ( echo WARN    Could not capture PID; will kill by name only. )

echo ==^> Polling %BASE%/api/health for up to 60s ...
set "READY="
for /L %%I in (1,1,60) do (
    if not defined READY (
        curl -sS --max-time 2 "%BASE%/api/health" >"%TEMP%\vta-health.txt" 2>NUL
        if not errorlevel 1 (
            findstr /C:"\"ok\"" /C:"ok" "%TEMP%\vta-health.txt" >NUL 2>NUL
            if not errorlevel 1 set "READY=1"
        )
        if defined READY (
            echo         ready after %%I sec.
        ) else (
            ping -n 2 127.0.0.1 >NUL
        )
    )
)
del "%TEMP%\vta-health.txt" 2>NUL
if not defined READY (
    echo FAIL    %BASE%/api/health never became ready in 60s.
    echo === Tail of recent console output (best-effort) ===
    if exist "%ROOT%logs\*.log" (
        for /f "delims=" %%F in ('dir /B /O-D "%ROOT%logs\*.log" 2^>NUL') do (
            if not defined READY (
                powershell -NoProfile -Command "Get-Content '%ROOT%logs\%%F' -Tail 40" 2>NUL
                set "READY=1"
            )
        )
    )
    goto :fail
)

REM ===== 7) Optional self-test =====
if "%SELFTEST%"=="1" (
    echo ==^> SELFTEST: HTTP section via selftest\selftest.bat ...
    if exist "%ROOT%selftest\selftest.bat" (
        call "%ROOT%selftest\selftest.bat" -Base "%BASE%" -Only http
        set "STR=!ERRORLEVEL!"
        if "!STR!"=="0" ( echo OK      HTTP self-test passed. ) else (
            echo FAIL    HTTP self-test exit !STR!.
            goto :fail
        )
    ) else ( echo WARN    selftest\selftest.bat missing; HTTP section SKIPPED. )
)

REM ===== 7) Keep alive =====
if defined KEEPALIVE (
    echo ==^> KEEPALIVE %KEEPALIVE%s ...
    set /a KP=%KEEPALIVE%+1
    ping -n !KP! 127.0.0.1 >NUL
) else (
    echo ==^> Service is running. Press ENTER to stop and cleanup.
    pause >NUL
)

echo OK  deploy-check.bat complete.
goto :end

:help
echo.
echo Usage: deploy-check.bat [/SELFTEST] [/PORT N] [/KEEPALIVE N] [/?]
echo   /SELFTEST          After ready, run HTTP multi-table self-test.
echo   /PORT 15232        Override health-check port (default: 15232).
echo   /KEEPALIVE N       Auto-exit after N seconds (CI / unattended mode).
echo.
endlocal
exit /b 0

:fail
echo.
echo FAIL  deploy-check.bat stopped.
call :cleanup
endlocal
exit /b 1

:end
call :cleanup
endlocal
exit /b 0

REM ==========================================================================
REM Sub: cleanup - kill processes started by THIS run (SIG-file fence),
REM                and (in SELFTEST) wipe aggregated tables/hotwords.
REM ==========================================================================
:cleanup
if "%PROC_RUN%"=="1" (
    echo ==^> Cleanup: stop VoiceTableAssist.exe and sherpa server ...
    if defined PID (
        taskkill /F /PID %PID% /T >NUL 2>&1
        ping -n 2 127.0.0.1 >NUL
        taskkill /F /PID %PID% /T >NUL 2>&1
    )
    taskkill /F /IM VoiceTableAssist.exe /T >NUL 2>&1
    taskkill /F /IM sherpa-onnx-online-websocket-server.exe /T >NUL 2>&1
    ping -n 3 127.0.0.1 >NUL
    taskkill /F /IM VoiceTableAssist.exe /T >NUL 2>&1
    taskkill /F /IM sherpa-onnx-online-websocket-server.exe /T >NUL 2>&1
) else (
    REM Start never happened; fence just in case.
    taskkill /F /IM VoiceTableAssist.exe /T >NUL 2>&1
    taskkill /F /IM sherpa-onnx-online-websocket-server.exe /T >NUL 2>&1
)

REM SELFTEST wipe extracted to :wipeTestData - keeps this subroutine free of
REM deeply nested if/for blocks that trip cmd 5.1's endlocal/exit boundary bug.
if "%SELFTEST%"=="1" call :wipeTestData

if exist "%SIG%" del /F /Q "%SIG%" 2>NUL
exit /b

REM ==========================================================================
REM Sub: wipeTestData - SELFTEST only: wipe aggregated tables + hotwords so the
REM      next run starts clean. Flat structure on purpose (no deep nesting).
REM ==========================================================================
:wipeTestData
echo ==^> SELFTEST: wipe aggregated tables + truncate hotwords ...
if exist "%ROOT%models\embedding\tables" rmdir /S /Q "%ROOT%models\embedding\tables" 2>NUL
if defined SHERPA_DIR (
    if exist "!SHERPA_DIR!\hr\tables" (
        for /D %%D in ("!SHERPA_DIR!\hr\tables\*") do (
            if /I not "%%~nxD"=="current" rmdir /S /Q "%%D" 2>NUL
        )
        break>"!SHERPA_DIR!\hr\tables\current\hotwords.txt"
    )
)
exit /b


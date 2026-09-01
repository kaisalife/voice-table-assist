@echo off
REM ============================================================================
REM  selftest.bat - HTTP / WS / ASR three-section self-test (pure batch)
REM  HTTP section: curl.exe against /import_table, /text_to_json,
REM                /api/speech/ner, /tables and /api/health. CJK payloads are
REM                sent as ASCII \uXXXX JSON escapes, so this BAT stays pure
REM                ASCII and the HTTP section needs no Node.js at all.
REM  WS  / ASR   : requires Node.js >=14 + npm package "ws"
REM                 (cd selftest && npm install ws)
REM  Missing dependencies cause the section to SKIP (not FAIL).
REM
REM  Usage (run AFTER starting VoiceTableAssist.exe):
REM    selftest.bat [-Base URL] [-Only http|ws|asr]
REM                 [-AsrTable NAME] [-Wav PATH]
REM                 [-ReadyTimeoutSec N] [-AsrTimeoutSec N]
REM    selftest.bat /?   this help
REM ============================================================================
REM Exit codes: 0 = all sections PASSED (SKIPs don't count)
REM             1 = any section FAILED
REM             2 = backend unreachable ^(-Only http/ws unreachable^)
REM ============================================================================
setlocal EnableExtensions EnableDelayedExpansion
REM Capture %~dp0 BEFORE the arg-parse loop: `shift` moves %0 as well, so
REM evaluating %~dp0 after parsing a switch would resolve to "D:\".
set "HERE=%~dp0"
chcp 65001 >nul
cd /d "%HERE%"

set "BASE=http://127.0.0.1:15232"
set "ONLY="
set "ASRTABLE="
set "WAV="
set "READY_TIMEOUT=5000"
set "ASR_TIMEOUT=30000"

:parse
if "%~1"=="" goto :doneParse
if /I "%~1"=="/?"          goto :help
if /I "%~1"=="-?"          goto :help
if /I "%~1"=="--help"      goto :help
if /I "%~1"=="-Base"       set "BASE=%~2"     & shift & shift & goto :parse
if /I "%~1"=="-base"       set "BASE=%~2"     & shift & shift & goto :parse
if /I "%~1"=="-Only"       set "ONLY=%~2"     & shift & shift & goto :parse
if /I "%~1"=="-only"       set "ONLY=%~2"     & shift & shift & goto :parse
if /I "%~1"=="-AsrTable"   set "ASRTABLE=%~2" & shift & shift & goto :parse
if /I "%~1"=="-asrTable"   set "ASRTABLE=%~2" & shift & shift & goto :parse
if /I "%~1"=="-Wav"        set "WAV=%~2"      & shift & shift & goto :parse
if /I "%~1"=="-wav"        set "WAV=%~2"      & shift & shift & goto :parse
if /I "%~1"=="-ReadyTimeoutSec" set "READY_TIMEOUT=%~2" & shift & shift & goto :parse
if /I "%~1"=="-AsrTimeoutSec"   set "ASR_TIMEOUT=%~2"   & shift & shift & goto :parse
shift
goto :parse
:doneParse

set "PASS=0"
set "FAIL=0"
set "TOTAL=0"
set "FAIL_NOW="
set "REQ=%TEMP%\vta-req-%RANDOM%.json"
set "RES=%TEMP%\vta-res-%RANDOM%.json"

REM ========= Prereq: curl + health probe =========
where curl >NUL 2>NUL
if errorlevel 1 ( echo FAIL  curl.exe not found on PATH (Win10 1809+ required). & goto :fail )
echo ==^> Health probe %BASE%/api/health ...
curl -sS --max-time 3 "%BASE%/api/health" -o "%RES%"
if errorlevel 1 (
    echo FAIL    Backend unreachable at %BASE%/api/health
    goto :unreachable
)
findstr /I "ok" "%RES%" >NUL
if errorlevel 1 (
    echo FAIL    /api/health not OK:
    type "%RES%"
    echo.
    goto :unreachable
)
echo OK

REM ========= Section switch =========
set "DO_HTTP=1"
set "DO_WS=1"
set "DO_ASR=1"
if /I "%ONLY%"=="http" ( set "DO_WS=" & set "DO_ASR=" )
if /I "%ONLY%"=="ws"   ( set "DO_HTTP=" & set "DO_ASR=" )
if /I "%ONLY%"=="asr"  ( set "DO_HTTP=" & set "DO_WS=" )

REM ========= Section 1: HTTP =========
if defined DO_HTTP (
    echo.
    echo ============================================================
    echo   Section 1: HTTP  (import_table / text_to_json / ner / tables)
    echo ============================================================
    call :assertHealth

    REM ---- [1/7] POST /import_table : default table 6x6, zh labels via \uXXXX ----
    echo.
    echo [1/7] POST /import_table - default 6x6 ...
    call :postJSON "%BASE%/import_table" "{\"rows\":[{\"label\":\"\u5916\u5F84\",\"index\":1},{\"label\":\"\u5185\u5F84\",\"index\":2},{\"label\":\"\u786C\u5EA6\",\"index\":3},{\"label\":\"\u5149\u6D01\u5EA6\",\"index\":4},{\"label\":\"\u76F4\u7EBF\u5EA6\",\"index\":5},{\"label\":\"\u5706\u5EA6\",\"index\":6}],\"columnCount\":6}"
    call :assertContains "\"status\":\"ok\"" "default import ok"

    REM ---- [2/7] POST /import_table : mechanical-properties table 3x4 ----
    echo.
    echo [2/7] POST /import_table - mech-properties 3x4 ...
    call :postJSON "%BASE%/import_table" "{\"tableName\":\"\u529B\u5B66\u6027\u80FD\",\"rows\":[{\"label\":\"\u6297\u62C9\u5F3A\u5EA6\",\"index\":1},{\"label\":\"\u5C48\u670D\u5F3A\u5EA6\",\"index\":2},{\"label\":\"\u4F38\u957F\u7387\",\"index\":3}],\"columnCount\":4}"
    call :assertContains "\"status\":\"ok\"" "mech import ok"

    REM ---- [3/7] GET /tables : both tables registered ----
    echo.
    echo [3/7] GET /tables - expect rowsCount 6 and 3 ...
    call :getURL "%BASE%/tables"
    call :assertContains "\"rowsCount\":6" "default rowsCount=6 listed"
    call :assertContains "\"rowsCount\":3" "mech rowsCount=3 listed"

    REM ---- [4/7] POST /text_to_json : mech table, tensile #1 = 300 -> (1,1) ----
    echo.
    echo [4/7] POST /text_to_json mech 'tensile #1 = 300' -^> (1,1)=300 ...
    call :postJSON "%BASE%/text_to_json" "{\"text\":\"\u6297\u62C9\u5F3A\u5EA6\u4E00\u53F7\u662F\u4E09\u767E\",\"table\":\"\u529B\u5B66\u6027\u80FD\"}"
    call :assertContains "\"row\":1" "mech hit row 1"
    call :assertContains "\"values\":300" "mech hit value 300"

    REM ---- [5/7] POST /text_to_json : switch to default, hardness #1 = 50 -> (3,1) ----
    echo.
    echo [5/7] POST /text_to_json default 'hardness #1 = 50' -^> (3,1)=50 ...
    call :postJSON "%BASE%/text_to_json" "{\"text\":\"\u786C\u5EA6\u4E00\u53F7\u662F\u4E94\u5341\",\"table\":\"default\"}"
    call :assertContains "\"row\":3" "default hit row 3 (hardness)"
    call :assertContains "\"values\":50" "default hit value 50"

    REM ---- [6/7] GET /api/health : activeTable switched to default ----
    echo.
    echo [6/7] GET /api/health - activeTable=default after switch ...
    call :getURL "%BASE%/api/health"
    call :assertContains "\"activeTable\":\"default\"" "health activeTable=default"

    REM ---- [7/7] POST /api/speech/ner : mech table, yield #2 = 200 -> triple (2,2) ----
    echo.
    echo [7/7] POST /api/speech/ner mech 'yield #2 = 200' -^> triple (2,2)=200 ...
    call :postJSON "%BASE%/api/speech/ner" "{\"text\":\"\u5C48\u670D\u5F3A\u5EA6\u4E8C\u53F7\u662F\u4E8C\u767E\",\"table\":\"\u529B\u5B66\u6027\u80FD\"}"
    call :assertContains "\"row\":2" "ner hit row 2"
    call :assertContains "\"value\":200" "ner hit value 200"
) else ( echo Section HTTP: SKIP (by -Only). )

REM ========= Section 2: WS ready latency =========
if defined DO_WS (
    echo.
    echo ============================================================
    echo   Section 2: WS ready latency (/api/speech/asr/stream)
    echo ============================================================
    call :ensureNodeWs
    if errorlevel 1 ( echo Section WS: SKIP (Node or ws missing). ) else (
        REM Single-connection gateway: probe the ASCII-named default table
        REM (CJK table names would need URL-encoding in the query string).
        set "INAME=default"
        set "WSU=!BASE:http=ws!/api/speech/asr/stream?table=!INAME!"
        echo ==^> node ws_client.js ready !WSU! !READY_TIMEOUT! ...
        node "%HERE%ws_client.js" ready "!WSU!" "!READY_TIMEOUT!" >"%TEMP%\vta-wsready.txt"
        set "WR=!ERRORLEVEL!"
        set "MS="
        for /f "useback tokens=*" %%L in ("%TEMP%\vta-wsready.txt") do set "MS=%%L"
        del "%TEMP%\vta-wsready.txt"
        if "!WR!"=="0" (
            set /A TEST=!MS!+0
            if !TEST! LEQ 3000 (
                echo OK      ready latency !MS! ms ^<= 3000ms.
                set /a PASS+=1
                set /a TOTAL+=1
            ) else (
                echo FAIL    ready latency !MS! ms ^> 3000ms.
                set /a FAIL+=1
                set /a TOTAL+=1
            )
        ) else ( echo FAIL    ws_client ready exit !WR!. & set /a FAIL+=1 & set /a TOTAL+=1 )
    )
) else ( echo Section WS: SKIP (by -Only). )

REM ========= Section 3: ASR e2e =========
if defined DO_ASR (
    echo.
    echo ============================================================
    echo   Section 3: ASR e2e (stream float32 PCM via WS and read cells)
    echo ============================================================
    if "%WAV%"=="" (
        echo SKIP    -Wav PATH is empty. Provide a 16kHz WAV file via:
        echo         selftest\convert-to-wav.bat some_audio.mp3 sample.wav
        echo         then rerun: selftest.bat -Wav sample.wav
    ) else (
        if not exist "%WAV%" ( echo FAIL    -Wav file not found: %WAV% & set /a FAIL+=1 & set /a TOTAL+=1 ) else (
            call :ensureNodeWs
            if errorlevel 1 ( echo Section ASR: SKIP (Node or ws missing). ) else (
                REM Table for the WS query string: -AsrTable overrides; default is ASCII-safe.
                set "TABLE=%ASRTABLE%"
                if "!TABLE!"=="" set "TABLE=default"
                set "PCM=%TEMP%\vta-asr-%RANDOM%.pcm"
                echo ==^> node text2float32.js "%WAV%" "!PCM!" ...
                node "%HERE%text2float32.js" "%WAV%" "!PCM!"
                set "TR=!ERRORLEVEL!"
                if not "!TR!"=="0" (
                    echo FAIL    text2float32 exit !TR!. & set /a FAIL+=1 & set /a TOTAL+=1
                ) else (
                    set "WSU=!BASE:http=ws!/api/speech/asr/stream?table=!TABLE!"
                    echo ==^> node ws_client.js asr "!WSU!" "!PCM!" !ASR_TIMEOUT! ...
                    node "%HERE%ws_client.js" asr "!WSU!" "!PCM!" "!ASR_TIMEOUT!" >"%TEMP%\vta-asrout.txt"
                    set "AR=!ERRORLEVEL!"
                    set "HASCELLS="
                    findstr /I "cells" "%TEMP%\vta-asrout.txt" >NUL 2>&1
                    if not errorlevel 1 set "HASCELLS=1"
                    type "%TEMP%\vta-asrout.txt"
                    del "%TEMP%\vta-asrout.txt"
                    del "!PCM!"
                    if "!AR!"=="0" ( if defined HASCELLS (
                        echo OK      ASR returned cells.
                        set /a PASS+=1 & set /a TOTAL+=1
                    ) else (
                        echo FAIL    ASR succeeded but no cells returned.
                        set /a FAIL+=1 & set /a TOTAL+=1
                    )) else ( echo FAIL    ws_client asr exit !AR!. & set /a FAIL+=1 & set /a TOTAL+=1 )
                )
            )
        )
    )
) else ( echo Section ASR: SKIP (by -Only). )

REM ========= Summary =========
echo.
echo ============================================================
echo   SELFTEST summary: TOTAL=%TOTAL%  PASS=%PASS%  FAIL=%FAIL%
echo ============================================================
if "%FAIL%"=="0" if "%TOTAL%"=="0" echo (no sections ran)
if "%FAIL%"=="0" ( goto :end ) else ( goto :fail )

REM ==========================================================================
REM Subroutines
REM ==========================================================================
:postJSON
set "U=%~1"
set "B=%~2"
REM Write JSON body to REQ file so we never cross cmd argument-code-page streams.
> "%REQ%" echo(%B%
curl -sS -X POST -H "Content-Type: application/json; charset=utf-8" --data-binary "@%REQ%" "%U%" -o "%RES%"
exit /b

:getURL
curl -sS "%~1" -o "%RES%"
exit /b

:assertContains
set "PAT=%~1"
set "MSG=%~2"
findstr /C:"%PAT%" "%RES%" >NUL
if errorlevel 1 (
    echo FAIL    %MSG%: substring "%PAT%" missing. response=
    type "%RES%"
    echo.
    set /a FAIL+=1
    set /a TOTAL+=1
) else (
    echo OK      %MSG%
    set /a PASS+=1
    set /a TOTAL+=1
)
exit /b

:assertHealth
curl -sS --max-time 3 "%BASE%/api/health" -o "%RES%"
if errorlevel 1 ( echo FAIL    /api/health unreachable in middle of self-test. & goto :unreachable )
findstr /I "ok" "%RES%" >NUL
if errorlevel 1 ( echo FAIL    /api/health not ok mid-self-test. & type "%RES%" & echo. & goto :fail )
exit /b

:ensureNodeWs
where node >NUL 2>NUL
if errorlevel 1 ( exit /b 1 )
REM verify ws can be loaded
node -e "require('ws')" 2>NUL
if errorlevel 1 (
    echo WARN    npm package 'ws' not installed; try: pushd "%HERE%" && npm install ws && popd
    exit /b 1
)
exit /b 0

:asrPortOK
REM  Quick TCP check for sherpa port (fallback: assume OK).
if "%ASR_PORT%"=="" set "ASR_PORT=6006"
call :ensureNodeWs
if errorlevel 1 ( exit /b 0 )
node -e "const n=require('net');const c=n.connect(%ASR_PORT%,'127.0.0.1',()=>{c.end();process.exit(0)});c.on('error',()=>process.exit(1));" 2>NUL
exit /b

:help
echo.
echo Usage: selftest.bat [options]
echo   -Base URL             Base URL of VoiceTableAssist (default http://127.0.0.1:15232)
echo   -Only {http^|ws^|asr}  Run only the named section.
echo   -AsrTable NAME        Active table name for ASR e2e section.
echo   -Wav PATH             16kHz PCM WAV file to stream in ASR e2e.
echo   -ReadyTimeoutSec N    WS ready timeout (default 5000 ms).
echo   -AsrTimeoutSec N      ASR stream deadline (default 30000 ms).
echo.
exit /b 0

:unreachable
del "%REQ%" 2>NUL
del "%RES%" 2>NUL
endlocal & exit /b 2

:fail
del "%REQ%" 2>NUL
del "%RES%" 2>NUL
endlocal & exit /b 1

:end
del "%REQ%" 2>NUL
del "%RES%" 2>NUL
endlocal & exit /b 0

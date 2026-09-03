@echo off
REM ============================================================================
REM  publish-linux.bat - Publish voice-table-assist for linux-x64
REM
REM  Linux deploy scripts are naturally bash. This BAT does:
REM    1) Print a ready-to-copy bash block that you can run in WSL / target host.
REM    2) Optionally: do a Windows-side dotnet cross-publish and zip, IF
REM       you put Linux ELF binaries of sherpa-onnx in .\sherpa-linux\ first.
REM
REM  Usage:
REM    publish-linux.bat                 bash script + optional cross publish
REM    publish-linux.bat /BashOnly       only print the bash script block
REM    publish-linux.bat /CrossOnly      only run Windows-side cross publish
REM    publish-linux.bat /?              this help
REM ============================================================================
setlocal EnableExtensions EnableDelayedExpansion
REM Capture %~dp0 BEFORE the arg-parse loop: `shift` moves %0 as well, so
REM evaluating %~dp0 after parsing a switch would resolve to "D:\".
set "PROJECT=%~dp0"
chcp 65001 >nul
cd /d "%PROJECT%"

set "CROSS=1"
set "BASH=1"
:parse
if "%~1"=="" goto :doneParse
if /I "%~1"=="/?"          goto :help
if /I "%~1"=="-?"          goto :help
if /I "%~1"=="--help"      goto :help
if /I "%~1"=="/CrossOnly"  set "BASH="   & shift & goto :parse
if /I "%~1"=="/BashOnly"   set "CROSS="  & shift & goto :parse
shift
goto :parse
:doneParse

set "OUT=%PROJECT%publish-linux\"
for %%D in ("%PROJECT%..\..") do set "ZIPDIR=%%~fD\publish"
set "ZIP=%ZIPDIR%\voice-table-assist-linux-x64.zip"

REM The bash block is emitted from a flat subroutine (NOT inside an if-block):
REM cmd 5.1 closes a parenthesized block at the first unquoted `)` in echo
REM text, so `$(...)` command substitutions in bash lines would break parsing
REM and force error-prone caret escaping that also pollutes the bash output.
if not defined BASH goto :skipBash
:bashblock
echo.
echo ==============================================================
echo   Linux publish - COPY this bash block into WSL/Linux host.
echo   Run it INSIDE the app/VoiceTableAssist directory.
echo ==============================================================
echo.
echo #!/bin/bash
echo set -euo pipefail
echo PROJECT="$$( cd "$$(dirname "$$0")" ^&^& pwd )"
echo OUT="$$PROJECT/publish-linux"
echo ZIP="$$( cd "$$PROJECT/../.." ^&^& pwd )/publish/voice-table-assist-linux-x64.zip"
echo.
echo test -d "$$PROJECT/models/raner"   ^|^| { echo "ERROR: models missing; run modelscope download first"; exit 1; }
echo test -d "$$PROJECT/sherpa-linux"   ^|^| echo "WARNING: $$PROJECT/sherpa-linux missing; ASR on deploy host will fail."
echo.
echo rm -rf "$$OUT"
echo dotnet publish -c Release -r linux-x64 --self-contained true -o "$$OUT"
echo mkdir -p "$$OUT/models"
echo cp -a "$$PROJECT/models/raner"        "$$OUT/models/"
echo cp -a "$$PROJECT/models/embedding"    "$$OUT/models/"
echo cp -a "$$PROJECT/models/asr"          "$$OUT/models/"
echo find  "$$OUT/models" -type f \
echo   \( -name '*.zip' -o -name '*.tar.bz2' -o -name '*.tar.gz' -o -name '*.tgz' \) \
echo   -delete
echo rm -rf "$$OUT/models/embedding/tables"
echo if [ -f "$$OUT/models/asr/gtcrn_simple.onnx" ]; then
echo   echo "OK: GTCRN denoise model bundled $$( du -k "$$OUT/models/asr/gtcrn_simple.onnx" | cut -f1 ) KB"
echo else
echo   echo "WARNING: GTCRN denoise model missing models/asr/gtcrn_simple.onnx (optional, factory-noise scenes)"
echo fi
echo.
echo if [ -d "$$PROJECT/sherpa-linux" ]; then
echo   mkdir -p "$$OUT/sherpa-onnx" ^&^& cp -a "$$PROJECT/sherpa-linux/." "$$OUT/sherpa-onnx/"
echo else
echo   mkdir -p "$$OUT/sherpa-onnx"
echo fi
echo mkdir -p "$$OUT/sherpa-onnx/hr/tables/current"
echo :^> "$$OUT/sherpa-onnx/hr/tables/current/hotwords.txt"
echo.
echo if [ -d "$$PROJECT/selftest" ]; then
echo   cp -a "$$PROJECT/selftest" "$$OUT/"
echo   rm -f  "$$OUT/selftest/"*.ps1
echo fi
echo [ -d "$$PROJECT/cordova"  ] ^&^& cp -a "$$PROJECT/cordova"  "$$OUT/"
echo mkdir -p "$$OUT/bundled-docs"
echo for f in deploy.md usage.md api.md; do
echo   [ -f "$$PROJECT/bundled-docs/$$f" ] ^&^& cp "$$PROJECT/bundled-docs/$$f" "$$OUT/bundled-docs/$$f"
echo done
echo.
echo mkdir -p "$$( dirname "$$ZIP" )"
echo rm -f "$$ZIP"
echo ( cd "$$OUT" ^&^& zip -qr "$$ZIP" . ) \
echo   ^|^| ( cd "$$OUT" ^&^& tar  -caf "$$ZIP" .  )
echo echo "DONE: $$ZIP"
echo.
echo ==============================================================
echo   After deploy on Linux:
echo     chmod +x VoiceTableAssist sherpa-onnx/*
echo     ./VoiceTableAssist   (or install a systemd unit; see docs).
echo ==============================================================
echo.
goto :skipBash
:skipBash

if defined CROSS (
    echo ==^> Cross-publishing on Windows host ^(win -^> linux-x64^) ...
    where dotnet >NUL 2>NUL
    if errorlevel 1 ( echo SKIP  dotnet not on PATH; cross-publish skipped. & goto :end )
    where tar    >NUL 2>NUL
    if errorlevel 1 ( echo WARN  tar missing; final zip step will be skipped. )

    echo ==^> Cleaning old publish-linux/ and zip ...
    if exist "%OUT%"  rmdir /S /Q "%OUT%"
    if exist "%ZIP%"  del   /F /Q "%ZIP%"

    echo ==^> dotnet publish -r linux-x64 --self-contained ...
    dotnet publish -c Release -r linux-x64 --self-contained true -o "%OUT%"
    if errorlevel 1 ( echo FAIL  dotnet publish failed. & goto :fail )

    echo ==^> Copying models ...
    if not exist "%PROJECT%models\raner" ( echo FAIL  models missing; run modelscope download first. & goto :fail )
    xcopy "%PROJECT%models" "%OUT%models\" /E /I /H /Y /Q
    for /R "%OUT%models" %%F in (*.zip *.tar.bz2 *.tar.gz *.tgz *.tar) do del /F /Q "%%F"
    if exist "%OUT%models\embedding\tables" rmdir /S /Q "%OUT%models\embedding\tables"

    echo ==^> Check GTCRN denoise model ...
    if exist "%OUT%models\asr\gtcrn_simple.onnx" (
        echo OK      GTCRN 降噪模型已随包：models/asr/gtcrn_simple.onnx
    ) else (
        echo WARN    GTCRN 模型未随包：models/asr/gtcrn_simple.onnx（工厂噪声场景建议补上；缺失时启动自动降级关闭降噪）
    )

    echo ==^> Copying sherpa-linux/ into sherpa-onnx/ ...
    if exist "%PROJECT%sherpa-linux" (
        xcopy "%PROJECT%sherpa-linux" "%OUT%sherpa-onnx\" /E /I /H /Y /Q
    ) else (
        echo WARN    %PROJECT%sherpa-linux\ missing. Deploy package ASR will be broken.
        mkdir "%OUT%sherpa-onnx" 2>NUL
    )
    if not exist "%OUT%sherpa-onnx\hr\tables\current" mkdir "%OUT%sherpa-onnx\hr\tables\current"
    break>"%OUT%sherpa-onnx\hr\tables\current\hotwords.txt"

    echo ==^> Copying selftest + docs ...
    if exist "%PROJECT%selftest" (
        xcopy "%PROJECT%selftest" "%OUT%selftest\" /E /I /H /Y /Q
        for %%F in (selftest.ps1 convert-to-wav.ps1 deploy-check.ps1) do if exist "%OUT%selftest\%%F" del /F /Q "%OUT%selftest\%%F"
    )
    if exist "%PROJECT%cordova"  xcopy "%PROJECT%cordova"  "%OUT%cordova\"  /E /I /H /Y /Q
    call :copy3docs "%PROJECT%" "%OUT%"

    where tar >NUL 2>NUL
    if not errorlevel 1 (
        echo ==^> Creating %ZIP% ...
        if not exist "%ZIPDIR%" mkdir "%ZIPDIR%"
        pushd "%OUT%"
        tar -a -c -f "%ZIP%" *
        set "RC=!ERRORLEVEL!"
        popd
        if "!RC!"=="0" (
            for %%F in ("%ZIP%") do echo OK      zip: %%~fF [%%~zF bytes]
        ) else ( echo FAIL  tar exited !RC!. & goto :fail )
    )
)

:end
echo.
echo publish-linux.bat finished.
endlocal & exit /b 0

:help
echo.
echo Usage: publish-linux.bat [/BashOnly ^| /CrossOnly] [/?]
echo   /BashOnly   Only print the bash block you can copy into WSL / Linux host.
echo   /CrossOnly  Only run the Windows-side dotnet cross-publish + tar zip.
echo   Default     Run both sections.
echo.
exit /b 0

REM ------------------------------------------------------------------
REM Sub :copy3docs SRCROOT DSTROOT
REM   Copy deploy/user/api markdown files out of the Chinese-named docs
REM   folder via keyword match; avoids embedding CJK literals that cmd
REM   would mis-tokenize under non-UTF-8 system code pages.
REM ------------------------------------------------------------------
:copy3docs
set "SRC=%~1"
set "DST=%~2"
set "SDIR="
for /D %%X in ("%SRC%*") do (
    if exist "%%~fX\api*.md"   set "SDIR=%%~fX"
)
if not defined SDIR exit /b 0
for /D %%X in ("%SDIR%") do set "DDIR=%DST%%%~nX"
if "%DDIR%"=="" exit /b 0
if not exist "%DDIR%" mkdir "%DDIR%" 2>NUL
REM Ship list carries CJK filenames; chcp 65001 is active at call time,
REM so for /f decodes each UTF-8 line correctly before copy.
set "LST=%SRC%publish-docs.lst"
if not exist "%LST%" (
    for %%F in ("%SDIR%\api*.md") do copy /Y "%%F" "%DDIR%" >NUL
    exit /b 0
)
for /f "usebackq delims=" %%L in ("%LST%") do (
    if exist "%SDIR%\%%L" (
        copy /Y "%SDIR%\%%L" "%DDIR%" >NUL
    ) else (
        echo WARN    doc missing: %SDIR%\%%L
    )
)
exit /b 0

:fail
echo.
echo FAIL  publish-linux.bat stopped. See messages above.
endlocal & exit /b 1

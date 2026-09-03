@echo off
REM ============================================================================
REM  c-test.bat - Build publish\ WITHOUT zip packaging (pure batch, no PS1)
REM  Same as publish.bat minus step 8 (tar) and step 9 (size report).
REM  Use it for local dev / CI smoke-test: produces a ready-to-package publish/
REM  so you can inspect, run, or later call publish.bat /ZipOnly to zip it.
REM
REM  Usage (in app\VoiceTableAssist directory):
REM    c-test.bat          dotnet publish + copy extras; output: %PROJECT%publish\
REM    c-test.bat /?       this help
REM
REM  Output: %PROJECT%publish\  (same layout publish.bat would zip)
REM ============================================================================
setlocal EnableExtensions EnableDelayedExpansion
REM Capture %~dp0 BEFORE the arg-parse loop: `shift` moves %0 as well, so
REM evaluating %~dp0 after parsing args would resolve to "D:\".
set "PROJECT=%~dp0"
chcp 65001 >nul
cd /d "%PROJECT%"

:parse
if "%~1"=="" goto :doneParse
if /I "%~1"=="/?"     goto :help
if /I "%~1"=="-?"     goto :help
if /I "%~1"=="--help" goto :help
shift
goto :parse
:doneParse

set "PUBLISH=%PROJECT%publish\"

echo ==============================================================
echo   Build publish/ (no zip)
echo     ROOT  : %PROJECT%
echo     OUT   : %PUBLISH%
echo ==============================================================

REM ===== 0) Prerequisite tools =====
where dotnet >NUL 2>NUL
if errorlevel 1 ( echo FAIL  dotnet not found on PATH. & goto :fail )

REM ===== 1) Clean old =====
echo ==^> Clean old publish/ ...
if exist "%PUBLISH%" rmdir /S /Q "%PUBLISH%"
mkdir "%PUBLISH%"

REM ===== 2) dotnet publish =====
echo ==^> dotnet publish -c Release -r win-x64 --self-contained true -o publish ...
dotnet publish -c Release -r win-x64 --self-contained true -o "%PUBLISH%"
if errorlevel 1 ( echo FAIL  dotnet publish exit !ERRORLEVEL!. & goto :fail )

REM ===== 3) Copy models (strip archives) =====
echo ==^> Copy models/ ...
if not exist "%PROJECT%models\raner" (
    echo FAIL    %PROJECT%models\raner missing. Run modelscope download first.
    goto :fail
)
xcopy "%PROJECT%models" "%PUBLISH%models\" /E /I /H /Y /Q
echo ==^> Prune embedded archives from bundled models/ ...
for /R "%PUBLISH%models" %%F in (*.zip *.tar.bz2 *.tar.gz *.tgz *.tar) do del /F /Q "%%F"
if exist "%PUBLISH%models\embedding\tables" rmdir /S /Q "%PUBLISH%models\embedding\tables"

REM ===== 3.5) GTCRN denoise model (optional) =====
echo ==^> Check GTCRN denoise model ...
if exist "%PUBLISH%models\asr\gtcrn_simple.onnx" (
    echo OK      GTCRN denoise model bundled: models/asr/gtcrn_simple.onnx
) else (
    echo WARN    GTCRN model missing: models/asr/gtcrn_simple.onnx - optional, only for factory-noise scenes.
)

REM ===== 4) sherpa hotwords placeholder =====
echo ==^> Create sherpa hotwords placeholder file ...
if exist "%PUBLISH%models\sherpa-onnx" set "SHERPA_OUT=%PUBLISH%models\sherpa-onnx"
if exist "%PUBLISH%sherpa-onnx"        set "SHERPA_OUT=%PUBLISH%sherpa-onnx"
if defined SHERPA_OUT (
    if not exist "!SHERPA_OUT!\hr\tables\current" mkdir "!SHERPA_OUT!\hr\tables\current"
    break>"!SHERPA_OUT!\hr\tables\current\hotwords.txt"
) else (
    echo WARN    No sherpa-onnx directory inside publish/ after dotnet publish.
    echo         If sherpa is not bundled as content, put binaries in %PROJECT%sherpa-onnx\
    echo         OR %PROJECT%models\sherpa-onnx\ then re-run c-test.bat.
)

REM ===== 5) deploy-check.bat + selftest/ + install.bat =====
echo ==^> Copy deploy-check.bat + install.bat + selftest/ ...
copy /Y "%PROJECT%deploy-check.bat" "%PUBLISH%deploy-check.bat" >NUL
if exist "%PROJECT%install.bat" copy /Y "%PROJECT%install.bat" "%PUBLISH%install.bat" >NUL
if exist "%PROJECT%selftest" (
    xcopy "%PROJECT%selftest" "%PUBLISH%selftest\" /E /I /H /Y /Q
    REM Remove any legacy ps1 scripts inside selftest dir.
    for %%F in (selftest.ps1 convert-to-wav.ps1 deploy-check.ps1) do (
        if exist "%PUBLISH%selftest\%%F" del /F /Q "%PUBLISH%selftest\%%F"
    )
)

REM ===== 6) vc_redist.x64.exe (optional) =====
echo ==^> Copy vc_redist.x64.exe ...
if exist "%PROJECT%vc_redist.x64.exe" (
    copy /Y "%PROJECT%vc_redist.x64.exe" "%PUBLISH%vc_redist.x64.exe" >NUL
) else (
    echo WARN    %PROJECT%vc_redist.x64.exe missing. Drop it in project root to bundle it.
    echo         URL: https://aka.ms/vs/17/release/vc_redist.x64.exe
)

REM ===== 7) Cordova template (optional) =====
if exist "%PROJECT%cordova" (
    echo ==^> Copy cordova/ ...
    xcopy "%PROJECT%cordova" "%PUBLISH%cordova\" /E /I /H /Y /Q
    REM Inside Cordova template, keep any scripts for now - separate build.
)

REM ===== 7b) HTTPS certs (optional - enables tablet access on port 15433) =====
if exist "%PROJECT%certs\gateway.pfx" (
    echo ==^> Copy certs/ - HTTPS enabled for tablets.
    xcopy "%PROJECT%certs" "%PUBLISH%certs\" /E /I /H /Y /Q
) else (
    echo WARN    certs\gateway.pfx missing - HTTPS disabled. Run make-cert.bat first.
)

REM ===== 8) Docs =====
echo ==^> Copy bundled docs (3 files) ...
call :copy3docs "%PROJECT%" "%PUBLISH%"

echo.
echo ==============================================================
echo   DONE  : %PUBLISH%
echo   Ready to package: run publish.bat /ZipOnly to zip publish\
echo   Ready to smoke-test: run %PUBLISH%deploy-check.bat /SELFTEST
echo ==============================================================
goto :end

:help
echo.
echo Usage: c-test.bat [/?]
echo   Build publish\ without zip packaging.
echo   Same as publish.bat minus the tar zip and size report.
echo   Output: %PROJECT%publish\
echo   Then:  publish.bat /ZipOnly      to package the existing publish\
echo           %PROJECT%publish\deploy-check.bat /SELFTEST   to smoke-test
echo.
exit /b 0

REM ------------------------------------------------------------------
REM Sub :copy3docs SRCROOT DSTROOT
REM   Locate bundled-docs subfolder(s) and copy the 3 shipped markdown
REM   files (deploy / user / api) by keyword-matching against every
REM   *.md candidate. Mirrors publish.bat to keep behavior identical.
REM   We intentionally avoid embedding CJK literals in the script body
REM   because cmd.exe reads it with the system code page BEFORE
REM   chcp 65001 takes effect.
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
echo FAIL  c-test.bat stopped. See messages above.
endlocal & exit /b 1

:end
endlocal & exit /b 0

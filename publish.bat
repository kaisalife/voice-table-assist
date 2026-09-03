@echo off
REM ============================================================================
REM  publish.bat - Create voice-table-assist win-x64 zip (pure batch, no PS1)
REM  Targets self-contained win-x64 + bundled sherpa-onnx (Windows binaries)
REM  ASR model: only float32 (higher accuracy); int8 models are NOT included.
REM
REM  Usage (in app\VoiceTableAssist directory):
REM    publish.bat                     dotnet publish + copy extras + zip
REM    publish.bat /ZipOnly            skip dotnet publish; use existing publish/
REM    publish.bat /OutDir PATH        override zip output folder.
REM    publish.bat /?                  this help
REM
REM  Output: <OutDir>\voice-table-assist-win-x64.zip
REM ============================================================================
setlocal EnableExtensions EnableDelayedExpansion
REM Capture %~dp0 BEFORE the arg-parse loop: `shift` moves %0 as well, so
REM evaluating %~dp0 after parsing "/ZipOnly" would resolve to "D:\".
set "PROJECT=%~dp0"
chcp 65001 >nul
cd /d "%PROJECT%"

set "ZIPONLY="
set "OUTDIR="
:parse
if "%~1"=="" goto :doneParse
if /I "%~1"=="/?"       goto :help
if /I "%~1"=="-?"       goto :help
if /I "%~1"=="--help"   goto :help
if /I "%~1"=="/ZipOnly" set "ZIPONLY=1"   & shift & goto :parse
if /I "%~1"=="/OutDir"  set "OUTDIR=%~2"  & shift & shift & goto :parse
shift
goto :parse
:doneParse

set "PUBLISH=%PROJECT%publish\"
set "ZIPNAME=voice-table-assist-win-x64.zip"
if not "%OUTDIR%"=="" (
    if not exist "%OUTDIR%" mkdir "%OUTDIR%"
    set "ZIPOUT=%OUTDIR%\%ZIPNAME%"
) else (
    for %%D in ("%PROJECT%..\..") do (
        set "OUTDIR=%%~fD\publish"
        if not exist "%%~fD\publish" mkdir "%%~fD\publish"
    )
    set "ZIPOUT=!OUTDIR!\%ZIPNAME%"
)

echo ==============================================================
echo   Publish %ZIPNAME%
echo     ROOT  : %PROJECT%
echo     OUT   : %PUBLISH%
echo     ZIP   : %ZIPOUT%
echo     ZIPONLY : %ZIPONLY%
echo ==============================================================

REM ===== 0) Prerequisite tools =====
if not "%ZIPONLY%"=="1" (
    where dotnet >NUL 2>NUL
    if errorlevel 1 ( echo FAIL  dotnet not found on PATH. & goto :fail )
)
where tar >NUL 2>NUL
if errorlevel 1 ( echo FAIL  tar.exe missing ^(Win10 1803+ required^). & goto :fail )

REM ===== 1) Clean old =====
if "%ZIPONLY%"=="1" (
    REM /ZipOnly re-packages the EXISTING publish/ - keep it, drop only the zip.
    if not exist "%PUBLISH%VoiceTableAssist.dll" (
        echo FAIL    /ZipOnly needs an existing publish\ from a full run first.
        goto :fail
    )
    echo ==^> Keep existing publish/ ; delete old zip only ...
    if exist "%ZIPOUT%" del /F /Q "%ZIPOUT%"
) else (
    echo ==^> Clean old publish/ + zip ...
    if exist "%PUBLISH%" rmdir /S /Q "%PUBLISH%"
    if exist "%ZIPOUT%"  del   /F /Q "%ZIPOUT%"
    mkdir "%PUBLISH%"
)

REM dotnet publish first (skip only if /ZipOnly and publish/ exists)
if not "%ZIPONLY%"=="1" (
    echo ==^> dotnet publish -c Release -r win-x64 --self-contained true -o publish ...
    REM 显式只发布主项目（不要让 SDK 在解 Solution 的时候把 tests\VoiceTableAssist.Tests 也算进去）
    dotnet publish "%PROJECT%VoiceTableAssist.csproj" -c Release -r win-x64 --self-contained true -o "%PUBLISH%"
    if errorlevel 1 ( echo FAIL  dotnet publish exit !ERRORLEVEL!. & goto :fail )
) else (
    echo SKIP  dotnet publish - /ZipOnly.
)

REM ===== 1.5) Defensive check: tests/ must never land in publish/ =====
REM 主 csproj 已显式 <Compile Remove="tests\**" /> + <Content Remove="tests\**" />；
REM 此处做一次兜底：若发现 publish\ 下出现 tests\ 路径，立刻中止并提示。
if exist "%PUBLISH%tests" (
    echo FAIL    publish\ 目录下发现 tests\ 子目录——测试代码被错误发布。
    echo         检查 VoiceTableAssist.csproj 的 Compile/Content/None Remove 项。
    goto :fail
)

REM ===== 2) Copy models (strip archives) =====
echo ==^> Copy models/ ...
if not exist "%PROJECT%models\raner" (
    echo FAIL    %PROJECT%models\raner missing. Run modelscope download first.
    goto :fail
)
xcopy "%PROJECT%models" "%PUBLISH%models\" /E /I /H /Y /Q
echo ==^> Prune embedded archives from bundled models/ ...
for /R "%PUBLISH%models" %%F in (*.zip *.tar.bz2 *.tar.gz *.tgz *.tar) do del /F /Q "%%F"
if exist "%PUBLISH%models\embedding\tables" rmdir /S /Q "%PUBLISH%models\embedding\tables"

REM ===== 2.5) GTCRN 降噪模型（可选） =====
REM GTCRN 522KB ONNX 放 models/asr/，已被 step 2 的 xcopy 整个 models/ 一并复制到发布目录。
REM 复制完后做一次存在性提示，让操作员一眼看到降噪模型是否随包带出。
echo ==^> Check GTCRN denoise model ...
if exist "%PUBLISH%models\asr\gtcrn_simple.onnx" (
    for %%F in ("%PUBLISH%models\asr\gtcrn_simple.onnx") do (
        set /a GTCRN_SIZE_KB=%%~zF / 1024 >NUL
    )
    echo        OK     GTCRN 降噪模型已随包：models\asr\gtcrn_simple.onnx ^(!GTCRN_SIZE_KB! KB^)
    echo                启用方式：appsettings.json ^<- AsrProvider.Denoise.Enabled = true
    echo                （DSP 尚未实现，当前 Denoise() 为直通，参考 部署文档.md "GTCRN 降噪"）
) else (
    echo        WARN   GTCRN 模型未找到：models\asr\gtcrn_simple.onnx
    echo                工厂噪声场景建议补上：from https://github.com/k2-fsa/sherpa-onnx/releases/download/speech-enhancement-models/gtcrn_simple.onnx
    echo                缺这个文件不影响部署/启动；仅当 Denoise.Enabled=true 时才需要。
)

REM ===== 3) sherpa hotwords placeholder =====
echo ==^> Create sherpa hotwords placeholder file ...
if exist "%PUBLISH%models\sherpa-onnx" set "SHERPA_OUT=%PUBLISH%models\sherpa-onnx"
if exist "%PUBLISH%sherpa-onnx"        set "SHERPA_OUT=%PUBLISH%sherpa-onnx"
if defined SHERPA_OUT (
    if not exist "!SHERPA_OUT!\hr\tables\current" mkdir "!SHERPA_OUT!\hr\tables\current"
    break>"!SHERPA_OUT!\hr\tables\current\hotwords.txt"
) else (
    echo WARN    No sherpa-onnx directory inside publish/ after dotnet publish.
    echo         If sherpa is not bundled as content, put binaries in %PROJECT%sherpa-onnx\
    echo         OR %PROJECT%models\sherpa-onnx\ then re-run publish.bat.
)

REM ===== 4) deploy-check.bat + selftest/ + install.bat =====
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

REM ===== 5) vc_redist.x64.exe (optional) =====
echo ==^> Copy vc_redist.x64.exe ...
if exist "%PROJECT%vc_redist.x64.exe" (
    copy /Y "%PROJECT%vc_redist.x64.exe" "%PUBLISH%vc_redist.x64.exe" >NUL
) else (
    echo WARN    %PROJECT%vc_redist.x64.exe missing. Drop it in project root to bundle it.
    echo         URL: https://aka.ms/vs/17/release/vc_redist.x64.exe
)

REM ===== 6) Cordova template (optional) =====
if exist "%PROJECT%cordova" (
    echo ==^> Copy cordova/ ...
    xcopy "%PROJECT%cordova" "%PUBLISH%cordova\" /E /I /H /Y /Q
    REM Inside Cordova template, keep any scripts for now - separate build.
)

REM ===== 6b) HTTPS certs (optional - enables tablet access on port 15433) =====
if exist "%PROJECT%certs\gateway.pfx" (
    echo ==^> Copy certs/ - HTTPS enabled for tablets.
    xcopy "%PROJECT%certs" "%PUBLISH%certs\" /E /I /H /Y /Q
) else (
    echo WARN    certs\gateway.pfx missing - HTTPS disabled. Run make-cert.bat first.
)

REM ===== 7) Docs =====
echo ==^> Copy bundled docs (3 files) ...
REM Copy bundled docs from the docs folder (3 files: deploy/user/api).
REM Use a quoted ASCII for /D style dir-name filter to keep cmd parse safe under non-UTF8 codepages.
call :copy3docs "%PROJECT%" "%PUBLISH%"

REM ===== 8) zip via tar.exe =====
echo ==^> Creating zip %ZIPOUT% ...
pushd "%PUBLISH%"
tar -a -c -f "%ZIPOUT%" *
set "TRC=%ERRORLEVEL%"
popd
if not "%TRC%"=="0" ( echo FAIL  tar exit %TRC%. & goto :fail )

REM ===== 9) Size report =====
set "ZMB=?"
for %%F in ("%ZIPOUT%") do set /a ZMB=%%~zF / 1048576 2>NUL
echo.
echo ==============================================================
echo   DONE  : %ZIPOUT%
echo   SIZE  : %ZMB% MiB
echo   USAGE :
echo         1. Unzip the archive into a clean empty directory.
echo         2. Run install.bat as administrator.
echo            (install.bat runs deploy-check.bat /SELFTEST internally.)
echo         3. Open http://127.0.0.1:15232/
echo ==============================================================

:end
endlocal & exit /b 0

:help
echo.
echo Usage: publish.bat [/ZipOnly] [/OutDir PATH] [/?]
echo   /ZipOnly         Skip dotnet publish; re-package an existing publish/.
echo   /OutDir PATH     Override zip output directory (default: ..\..\publish).
echo   Example:
echo     publish.bat
echo     publish.bat /OutDir D:\release
echo     publish.bat /ZipOnly /OutDir D:\release
echo.
exit /b 0

REM ------------------------------------------------------------------
REM Sub :copy3docs SRCROOT DSTROOT
REM   Locate bundled-docs subfolder(s) and copy the 3 shipped markdown
REM   files (deploy / user / api) by keyword-matching against every
REM   *.md candidate. We intentionally avoid embedding CJK literals in
REM   the script body because cmd.exe reads it with the system code
REM   page BEFORE chcp 65001 takes effect.
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
echo FAIL  publish.bat stopped. See messages above.
endlocal & exit /b 1

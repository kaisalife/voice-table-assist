@echo off
REM ============================================================================
REM  convert-to-wav.bat - Any audio -> 16kHz/16bit/mono PCM WAV
REM  Looks up ffmpeg.exe on PATH or in script dir or parent dir.
REM  If missing, offers one-click install via winget / choco / gyan.dev URL.
REM
REM  Usage:
REM    convert-to-wav.bat  input.m4a               -> output: input.wav (same folder)
REM    convert-to-wav.bat  some.mp3 out.wav        -> output: out.wav
REM    convert-to-wav.bat  /?                      this help
REM ============================================================================
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul

if "%~1"=="" goto :help
if /I "%~1"=="/?" goto :help
if /I "%~1"=="-?" goto :help

set "IN=%~f1"
if not exist "%IN%" ( echo FAIL    input audio not found: %IN% & goto :fail )

set "OUT=%~f2"
if "%OUT%"=="" (
    for %%F in ("%IN%") do set "OUT=%%~dpnF.wav"
)
if /I "%IN%"=="%OUT%" ( echo FAIL    input and output path are the same. & goto :fail )

REM Locate ffmpeg.exe
set "FF="
where ffmpeg >NUL 2>NUL
if not errorlevel 1 for /f "delims=" %%F in ('where ffmpeg 2^>NUL') do set "FF=%%F" & goto :haveFfmpeg
if exist "%~dp0ffmpeg.exe"       set "FF=%~dp0ffmpeg.exe"       & goto :haveFfmpeg
if exist "%~dp0..\ffmpeg.exe"    set "FF=%~dp0..\ffmpeg.exe"    & goto :haveFfmpeg
:tryInstallFfmpeg
echo.
echo ffmpeg.exe not found. One-shot install option:
echo   1. winget install Gyan.FFmpeg.Essentials      (built-in on Win11 / AppInstaller)
echo   2. choco  install ffmpeg                      (if you have choco)
echo   3. https://www.gyan.dev/ffmpeg/builds/   -> essentials zip; place ffmpeg.exe next to this script.
echo.
where winget >NUL 2>NUL
if not errorlevel 1 (
    set /P "A=Run 'winget install -e --id Gyan.FFmpeg.Essentials --silent' now? [y/N] "
    if /I "!A!"=="y" (
        winget install -e --id Gyan.FFmpeg.Essentials --silent
        where ffmpeg >NUL 2>NUL
        if not errorlevel 1 (
            for /f "delims=" %%F in ('where ffmpeg 2^>NUL') do set "FF=%%F" & goto :haveFfmpeg
        )
        echo WARN    winget install may have succeeded but PATH is stale in this shell. Try a new cmd window.
    )
)
echo ffmpeg still unavailable. Install ffmpeg and rerun.
goto :fail

:haveFfmpeg
echo ==^> ffmpeg=%FF%
echo ==^> Converting to WAV PCM s16le / 16kHz / mono ...
"%FF%" -y -loglevel error -i "%IN%" -acodec pcm_s16le -ar 16000 -ac 1 "%OUT%"
if errorlevel 1 ( echo FAIL    ffmpeg exit !ERRORLEVEL!. & goto :fail )
echo OK      wrote %OUT%

REM Optional duration probe via ffprobe if available
set "FP="
for %%F in ("%FF%") do set "FP=%%~dpFffprobe.exe"
if exist "%FP%" (
    "%FP%" -v error -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 "%OUT%" 2>NUL
)

endlocal & exit /b 0

:help
echo.
echo Usage: convert-to-wav.bat ^<input_audio^> [output.wav]
echo   output.wav defaults to ^<input_stem^>.wav in the same folder.
echo   Output container is WAV, codec PCM s16le, 16kHz, mono.
echo.
echo ffmpeg.exe is required. One-click install:
echo   winget install Gyan.FFmpeg.Essentials
echo.
exit /b 0

:fail
endlocal & exit /b 1

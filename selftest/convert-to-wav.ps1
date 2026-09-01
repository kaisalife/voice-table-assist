# 用 Windows 内置 MediaTranscoder 把任意音频（m4a/mp3/...）转成 16k/16bit/mono PCM wav。
# 不依赖 ffmpeg，适合无法从 GitHub 下载 ffmpeg 的环境。
# Usage: .\convert-to-wav.ps1 -Source "录音.m4a" [-Output "录音.wav"]
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [string]$Output
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Runtime.WindowsRuntime

# WinRT 类型
$null = [Windows.Storage.StorageFile, Windows.Storage, ContentType = WindowsRuntime]
$null = [Windows.Storage.StorageFolder, Windows.Storage, ContentType = WindowsRuntime]
$null = [Windows.Storage.CreationCollisionOption, Windows.Storage, ContentType = WindowsRuntime]
$null = [Windows.Media.Transcoding.MediaTranscoder, Windows.Media.Transcoding, ContentType = WindowsRuntime]
$null = [Windows.Media.Transcoding.PrepareTranscodeResult, Windows.Media.Transcoding, ContentType = WindowsRuntime]
$null = [Windows.Media.MediaProperties.MediaEncodingProfile, Windows.Media.MediaProperties, ContentType = WindowsRuntime]
$null = [Windows.Media.MediaProperties.AudioEncodingProperties, Windows.Media.MediaProperties, ContentType = WindowsRuntime]
$null = [Windows.Media.MediaProperties.AudioEncodingQuality, Windows.Media.MediaProperties, ContentType = WindowsRuntime]

# AsTask(IAsyncOperation<TResult>) 泛型重载
$asTaskOp = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
        $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
    })[0]

# AsTask<TProgress>(IAsyncActionWithProgress<TProgress>)
$asTaskActionProg = ([System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
        $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and
        $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncActionWithProgress`1'
    })[0]

function Await-Operation($op, $resultType) {
    $m = $asTaskOp.MakeGenericMethod($resultType)
    $task = $m.Invoke($null, @($op))
    $task.Wait(-1) | Out-Null
    return $task.Result
}

$srcPath = (Resolve-Path -LiteralPath $Source).Path
if (-not $Output) { $Output = [IO.Path]::ChangeExtension($srcPath, '.wav') }
$outFull = [IO.Path]::GetFullPath($Output)
$outDir  = Split-Path -Parent $outFull
$outName = Split-Path -Leaf $outFull

Write-Host "INFO  输入: $srcPath"
Write-Host "INFO  输出: $outFull"

$src = Await-Operation ([Windows.Storage.StorageFile]::GetFileFromPathAsync($srcPath)) ([Windows.Storage.StorageFile])
$folder = Await-Operation ([Windows.Storage.StorageFolder]::GetFolderFromPathAsync($outDir)) ([Windows.Storage.StorageFolder])
$dst = Await-Operation ($folder.CreateFileAsync($outName, [Windows.Storage.CreationCollisionOption]::ReplaceExisting)) ([Windows.Storage.StorageFile])

# WAV 容器 + 16k/16bit/mono PCM 音频
$profile = [Windows.Media.MediaProperties.MediaEncodingProfile]::CreateWav([Windows.Media.MediaProperties.AudioEncodingQuality]::Auto)
$profile.Audio = [Windows.Media.MediaProperties.AudioEncodingProperties]::CreatePcm(16000, 1, 16)
$profile.Video = $null

$transcoder = New-Object Windows.Media.Transcoding.MediaTranscoder
$prep = Await-Operation ($transcoder.PrepareFileTranscodeAsync($src, $dst, $profile)) ([Windows.Media.Transcoding.PrepareTranscodeResult])

if (-not $prep.CanTranscode) {
    Write-Host "FAIL  无法转码（源格式不支持或解码器缺失），FailureReason=$($prep.FailureReason)"
    exit 1
}

# TranscodeAsync 返回 IAsyncActionWithProgress<double>（进度类型为 double）
$transcodeOp = $prep.TranscodeAsync()
$m = $asTaskActionProg.MakeGenericMethod([double])
$task = $m.Invoke($null, @($transcodeOp))
$task.Wait(-1) | Out-Null

Write-Host "OK    转码完成: $outFull"

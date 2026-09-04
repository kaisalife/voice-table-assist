# ============================================================================
# Cordova 安卓壳构建辅助（PS 5.1+）。
#
# 原理：APK 的 WebView 加载壳内本地页面（origin=http://localhost，安全上下文
#       → getUserMedia 麦克风可用），HTTP/WS 全部指向局域网 VoiceTableAssist 网关。
#       因此壳内页面必须与网关 wwwroot 保持同源 —— 本脚本从网关项目"单一来源"拷贝，
#       避免两份页面漂移。
#
# 用法（在本目录 cordova/）：
#   powershell -ExecutionPolicy Bypass -File .\build.ps1          # 同步 www/ + 环境检查
#   powershell -ExecutionPolicy Bypass -File .\build.ps1 -Build   # 同步后执行 cordova build android
#
# 一次性前置（在 cordova/ 目录）：
#   npm install -g cordova      # Cordova CLI
#   npm install                 # 安装 cordova-android 平台包（本目录 package.json）
#
# 产物：platforms/android/app/build/outputs/apk/debug/app-debug.apk
# 首次运行后，APK 里页面顶部"网关地址"填局域网地址（如 http://192.168.1.10:15232），
# 应用会记住（localStorage）。
# ============================================================================
param([switch]$Build)

$ErrorActionPreference = 'Stop'
$cordovaDir = $PSScriptRoot
$wwwroot    = Join-Path $cordovaDir '..\wwwroot'   # cordova/ 与 VoiceTableAssist 工程同级，wwwroot 就在上级
$www        = Join-Path $cordovaDir 'www'

# ---- 1) 同步前端三件套：页面 + 麦克风采集库 + AudioWorklet ----
$files = @('index.html', 'voice-mic.js', 'audio-capture-worklet.js')
New-Item -ItemType Directory -Force -Path $www | Out-Null
foreach ($f in $files) {
    $src = Join-Path $wwwroot $f
    if (-not (Test-Path $src)) { Write-Error "未找到 $src（请确认在 VoiceTableAssist 工程内运行）" }
    Copy-Item -Force $src (Join-Path $www $f)
    Write-Host "OK    已同步 www/$f"
}

# ---- 2) 环境检查与打包 ----
$cli = Get-Command cordova -ErrorAction SilentlyContinue
if (-not $cli) {
    Write-Warning '未检测到 cordova CLI（npm install -g cordova）。已同步 www/，跳过打包。'
    exit 0
}
if (-not (Test-Path (Join-Path $cordovaDir 'node_modules'))) {
    Write-Warning '未安装平台包（npm install）。已同步 www/，跳过打包。'
    exit 0
}

if ($Build) {
    # ---- 2a) 构建环境自动探测（本机一次性铺过：SDK=D:\Android\Sdk，Gradle=D:\Android\gradle-8.14.2，JDK=Android Studio 自带 JBR）----
    if (-not $env:ANDROID_HOME -and (Test-Path 'D:\Android\Sdk\platform-tools\adb.exe')) { $env:ANDROID_HOME = 'D:\Android\Sdk' }
    if (-not $env:ANDROID_SDK_ROOT -and $env:ANDROID_HOME) { $env:ANDROID_SDK_ROOT = $env:ANDROID_HOME }
    if (-not $env:JAVA_HOME -or -not (Test-Path "$env:JAVA_HOME\bin\java.exe")) {
        $jbr = 'C:\Program Files\Android\Android Studio\jbr'
        if (Test-Path "$jbr\bin\java.exe") { $env:JAVA_HOME = $jbr }
    }
    $gradleBin = 'D:\Android\gradle-8.14.2\bin'
    if (Test-Path "$gradleBin\gradle.bat") { $env:PATH = "$gradleBin;$env:JAVA_HOME\bin;$env:PATH" }
    # cordova-android 15 的 gradle wrapper 会联网校验 distribution URL，国内直连 services.gradle.org 易超时；
    # 用本地已下载的 gradle zip 覆盖，绕开网络。
    $localGradleZip = 'D:\Android\gradle-8.14.2-bin.zip'
    if ((Test-Path $localGradleZip) -and -not $env:CORDOVA_ANDROID_GRADLE_DISTRIBUTION_URL) {
        $env:CORDOVA_ANDROID_GRADLE_DISTRIBUTION_URL = 'file:///' + ($localGradleZip -replace '\\','/')
    }

    Push-Location $cordovaDir
    try {
        cordova build android
        if ($LASTEXITCODE -ne 0) { Write-Error 'cordova build android 失败' }
    } finally { Pop-Location }
} else {
    Write-Host '==> www/ 已同步。执行打包：powershell -ExecutionPolicy Bypass -File .\build.ps1 -Build'
    Write-Host '    （或手动：cordova build android）'
}

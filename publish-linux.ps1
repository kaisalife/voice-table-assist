# 打包发布 Linux：(self-contained linux-x64) + 模型 + Linux sherpa-onnx + wwwroot，打一个 zip。
#
# 前置：Windows 上装有 .NET 8 SDK 且能对 linux-x64 交叉发布；
#       另需准备 Linux 版 sherpa-onnx（官方 release 的 sherpa-onnx-vX-linux-x64.tar.bz2)，
#       解包后整目录放到本脚本所在目录下的 sherpa-linux/（含 bin/sherpa-onnx-online-websocket-server 与 models/、hr/）。
#
# 用法（在 app/VoiceTableAssist 目录）：
#   powershell -ExecutionPolicy Bypass -File .\publish-linux.ps1
# 说明：本脚本只做"发布+拷贝+打包"，不会改动 Windows 版 publish.ps1 的产物。
# 交付物：app\publish\voice-table-assist-linux-x64.zip（与 Windows 版同目录，不在仓库根）

$ErrorActionPreference = 'Stop'

$project   = $PSScriptRoot
$runtime   = 'linux-x64'
$outDir    = Join-Path $project 'publish-linux'
$modelsSrc = Join-Path $project 'models'
$sherpaLx  = Join-Path $project 'sherpa-linux'
$zipName   = 'voice-table-assist-linux-x64.zip'
# zip 放在 app\publish\ 下，与 Windows 版一致（仓库根目录可能受限不可写）
$zipDir    = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\publish'))
$zipOut    = Join-Path $zipDir $zipName

Write-Host "==> 清除旧的 publish-linux/zip"
if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir }
if (Test-Path $zipOut) { Remove-Item -Force $zipOut }

Write-Host "==> dotnet publish (self-contained $runtime)"
Push-Location $project
try {
    dotnet publish -c Release -r $runtime --self-contained true -o $outDir
    if ($LASTEXITCODE -ne 0) { throw 'dotnet publish 失败' }
}
finally { Pop-Location }

Write-Host '==> 拷贝模型资源 models/ (raner + embedding + asr)'
New-Item -ItemType Directory -Force -Path (Join-Path $outDir 'models') | Out-Null
Copy-Item -Recurse -Force (Join-Path $modelsSrc 'raner')     (Join-Path $outDir 'models\raner')
Copy-Item -Recurse -Force (Join-Path $modelsSrc 'embedding') (Join-Path $outDir 'models\embedding')
if (Test-Path (Join-Path $modelsSrc 'asr')) {
    Copy-Item -Recurse -Force (Join-Path $modelsSrc 'asr')   (Join-Path $outDir 'models\asr')
} else {
    throw "未找到 $(Join-Path $modelsSrc 'asr')。ASR 模型必须并入 models/asr/（与 Windows 版一致），否则服务启动后 ASR 不可用。"
}

# GTCRN 降噪模型（可选，522KB）：随 models/asr 整体拷贝自动带上，这里做存在性提示
$gtcrn = Join-Path $outDir 'models\asr\gtcrn_simple.onnx'
if (Test-Path $gtcrn) {
    $kb = [math]::Round((Get-Item $gtcrn).Length / 1KB)
    Write-Host "==> GTCRN 降噪模型已随包：models/asr/gtcrn_simple.onnx ($kb KB)"
} else {
    Write-Warning 'GTCRN 模型未随包：models/asr/gtcrn_simple.onnx（工厂噪声场景建议补上；缺失时启动自动降级关闭降噪）'
}

# 剔除"安卓化更新引入、C# 网关用不到"的模型文件（源码 models/ 不动，只清发布副本）：
#   int8 模型（安卓端）、silero_vad.onnx（安卓端 VAD）、models/sherpa-onnx/hr（安卓字典/热词目录）
foreach ($p in @(
        (Join-Path $outDir 'models\asr\silero_vad.onnx'),
        (Join-Path $outDir 'models\sherpa-onnx\hr')
    )) {
    if (Test-Path $p) { Remove-Item -Recurse -Force $p; Write-Host "==> 已剔除安卓化模型文件（不让入包）: $($p.Replace($outDir + '\', ''))" }
}
Get-ChildItem (Join-Path $outDir 'models\asr') -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like '*int8*' } |
    ForEach-Object { Remove-Item -Recurse -Force $_.FullName; Write-Host "==> 已剔除安卓 int8 模型目录: $($_.Name)" }

Write-Host "==> 拷贝 Linux sherpa-onnx（来自 sherpa-linux/）"
if (Test-Path $sherpaLx) {
    New-Item -ItemType Directory -Force -Path (Join-Path $outDir 'sherpa-onnx') | Out-Null
    Copy-Item -Recurse -Force (Join-Path $sherpaLx '*') (Join-Path $outDir 'sherpa-onnx')
    # 进程内识别不需要 server exe；若 sherpa-linux/ 里混了 Windows 旧文件也一并剔除
    Get-ChildItem (Join-Path $outDir 'sherpa-onnx') -Recurse -File |
        Where-Object { $_.Name -in @('sherpa-onnx-online-websocket-server.exe', 'sherpa-onnx-online-websocket-server') } |
        ForEach-Object { Remove-Item -Force $_.FullName; Write-Host "==> 已剔除 server exe: $($_.Name)" }
} else {
    Write-Warning "未找到 $sherpaLx。请在发布前放置 Linux 版 sherpa-onnx 原生库（libsherpa-onnx-c-api.so + libonnxruntime.so + hr/），否则 ASR 不可用。"
    New-Item -ItemType Directory -Force -Path (Join-Path $outDir 'sherpa-onnx') | Out-Null
}

# wwwroot 验证页已由 dotnet publish 自动包含。
# 识别在网关进程内（P/Invoke）：目标机需有 Linux 版 .so（Windows 的 .dll 不可用），
# 路径由 appsettings.json 的 SherpaServer:NativeDir 指定（默认 models/sherpa-onnx）。

# 附带部署检查脚本与多表自测脚本（目标机需 pwsh 运行；临时拉起验证，关掉脚本即停）
Copy-Item -Force (Join-Path $project 'deploy-check.ps1') (Join-Path $outDir 'deploy-check.ps1')
Copy-Item -Recurse -Force (Join-Path $project 'selftest') (Join-Path $outDir 'selftest')
# 证书相关（make-cert.*、certs/）不入包：平板走 Cordova 壳内的 http://localhost 安全上下文，无需 HTTPS 证书。

# 附带安卓 Cordova 壳模板（只带壳源；不含 platforms/node_modules 构建产物）
if (Test-Path (Join-Path $project 'cordova')) {
    $cordovaDst = Join-Path $outDir 'cordova'
    if (Test-Path $cordovaDst) { Remove-Item -Recurse -Force $cordovaDst }
    New-Item -ItemType Directory -Force -Path $cordovaDst | Out-Null
    foreach ($item in 'config.xml', 'package.json', 'package-lock.json', 'build.ps1', 'www', 'hooks', 'plugins') {
        $src = Join-Path $project ('cordova\' + $item)
        if (Test-Path $src) { Copy-Item -Recurse -Force $src (Join-Path $cordovaDst $item) }
    }
    Write-Host '==> 已附带 cordova 壳模板（不含 platforms/node_modules）'
}

# 附带文档：目标机部署运维直接看包内部署文档，无需回仓库翻
$docsSrc = Join-Path $project '相关文档'
if (Test-Path $docsSrc) {
    New-Item -ItemType Directory -Force -Path (Join-Path $outDir '相关文档') | Out-Null
    Copy-Item -Force (Join-Path $docsSrc '部署文档.md')   (Join-Path $outDir '相关文档\部署文档.md')
    Copy-Item -Force (Join-Path $docsSrc '用户使用指南.md') (Join-Path $outDir '相关文档\用户使用指南.md')
    Copy-Item -Force (Join-Path $docsSrc 'api文档.md')    (Join-Path $outDir '相关文档\api文档.md')
}

Write-Host '==> 压缩 zip'
New-Item -ItemType Directory -Force -Path $zipDir | Out-Null
Compress-Archive -Path (Join-Path $outDir '*') -DestinationPath $zipOut -Force

Write-Host "==> 完成: $zipOut"
Write-Host "解压后目录布局：VoiceTableAssist + models/{raner,embedding,asr} + sherpa-onnx/ + wwwroot/ + appsettings.json + deploy-check.ps1 + selftest/ + 相关文档/"
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

Write-Host "==> 拷贝 Linux sherpa-onnx（来自 sherpa-linux/）"
if (Test-Path $sherpaLx) {
    New-Item -ItemType Directory -Force -Path (Join-Path $outDir 'sherpa-onnx') | Out-Null
    Copy-Item -Recurse -Force (Join-Path $sherpaLx '*') (Join-Path $outDir 'sherpa-onnx')
} else {
    Write-Warning "未找到 $sherpaLx。请在发布前放置 Linux 版 sherpa-onnx（server + models + hr），否则 ASR 不可用。"
    New-Item -ItemType Directory -Force -Path (Join-Path $outDir 'sherpa-onnx') | Out-Null
}

# wwwroot 验证页已由 dotnet publish 自动包含。
# 部署时请相应修改 appsettings.json 的 SherpaServer:ExePath 为 Linux 版可执行文件名。

# 附带部署检查脚本与多表自测脚本（目标机需 pwsh 运行；临时拉起验证，关掉脚本即停）
Copy-Item -Force (Join-Path $project 'deploy-check.ps1') (Join-Path $outDir 'deploy-check.ps1')
Copy-Item -Recurse -Force (Join-Path $project 'selftest') (Join-Path $outDir 'selftest')

# 附带安卓 Cordova 壳模板（config.xml + package.json + build.ps1 + 已同步的 www/）
if (Test-Path (Join-Path $project 'cordova')) {
    Copy-Item -Recurse -Force (Join-Path $project 'cordova') (Join-Path $outDir 'cordova')
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
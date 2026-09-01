# 打包发布：self-contained win-x64 + 模型/sherpa hr/wwwroot，打一个 zip。
# ASR 模型仅保留 float32 版（识别精度更高）。int8 已从 models/asr 移除。
# 用法（在 app/VoiceTableAssist 目录）：
#   powershell -ExecutionPolicy Bypass -File .\publish.ps1
# 交付物：仓库根的 voice-table-assist-win-x64.zip

param()

$ErrorActionPreference = 'Stop'

$project   = $PSScriptRoot
$root      = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\'))
$publish   = Join-Path $project 'publish'
$modelsSrc = Join-Path $project 'models'
$zipName   = 'voice-table-assist-win-x64.zip'
# zip 放在 app\publish\ 下（而非仓库根，避免占用受限/不便查找）
$zipDir    = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\publish'))
$zipOut    = Join-Path $zipDir $zipName

Write-Host "==> 清除旧的 publish/zip"
if (Test-Path $publish) { Remove-Item -Recurse -Force $publish }
if (Test-Path $zipOut)  { Remove-Item -Force $zipOut }

Write-Host '==> dotnet publish (self-contained win-x64)'
Push-Location $project
try {
    dotnet publish -c Release -r win-x64 --self-contained true -o $publish
    if ($LASTEXITCODE -ne 0) { throw 'dotnet publish 失败' }
}
finally { Pop-Location }

Write-Host '==> 拷贝模型资源 models/ (raner + embedding + asr)'
New-Item -ItemType Directory -Force -Path (Join-Path $publish 'models') | Out-Null
Copy-Item -Recurse -Force (Join-Path $modelsSrc '*') (Join-Path $publish 'models')
# 模型原始压缩包（下载原件，如 asr 的 *.zip）运行不需要，不入包（单此一项省 550MB+）
Get-ChildItem (Join-Path $publish 'models') -Recurse -File -Include *.zip, *.tar.bz2, *.tar.gz |
    Remove-Item -Force

# 交付包不带运行期用户数据（表注册表 registry.json / 各表向量索引）：
# 首次启动服务后由前端初始化自动导入建库，避免旧机器的表状态随包污染新部署
$embTables = Join-Path $publish 'models\embedding\tables'
if (Test-Path $embTables) { Remove-Item -Recurse -Force $embTables }

# sherpa-onnx 原生运行时已归档在 models/sherpa-onnx（随上面 models 拷贝自动带上，ExePath 指向此处）；
# 这里只补 hr 热词占位：sherpa 启动要求热词文件存在（缺失会直接退出，服务端亦有自愈创建）
$hrCurrent = Join-Path $publish 'sherpa-onnx\hr\tables\current'
New-Item -ItemType Directory -Force -Path $hrCurrent | Out-Null
if (-not (Test-Path (Join-Path $hrCurrent 'hotwords.txt'))) {
    [System.IO.File]::WriteAllText((Join-Path $hrCurrent 'hotwords.txt'), '', [System.Text.UTF8Encoding]::new($false))
    Write-Host '==> 已预置空热词文件 sherpa-onnx/hr/tables/current/hotwords.txt'
}

# ASR 模型仅 float32（int8 已移除）。

# wwwroot 验证页已由 dotnet publish 自动包含（Web SDK 默认 Content）。
# 附带部署检查脚本与多表自测脚本（临时拉起验证，关掉脚本即停）。
Copy-Item -Force (Join-Path $project 'deploy-check.ps1') (Join-Path $publish 'deploy-check.ps1')
Copy-Item -Recurse -Force (Join-Path $project 'selftest') (Join-Path $publish 'selftest')

# 附带安卓 Cordova 壳模板（config.xml + package.json + build.ps1 + 已同步的 www/）
if (Test-Path (Join-Path $project 'cordova')) {
    Copy-Item -Recurse -Force (Join-Path $project 'cordova') (Join-Path $publish 'cordova')
}

# 附带文档：目标机部署运维直接看包内部署文档，无需回仓库翻
$docsSrc = Join-Path $project '相关文档'
if (Test-Path $docsSrc) {
    New-Item -ItemType Directory -Force -Path (Join-Path $publish '相关文档') | Out-Null
    Copy-Item -Force (Join-Path $docsSrc '部署文档.md')   (Join-Path $publish '相关文档\部署文档.md')
    Copy-Item -Force (Join-Path $docsSrc '用户使用指南.md') (Join-Path $publish '相关文档\用户使用指南.md')
    Copy-Item -Force (Join-Path $docsSrc 'api文档.md')    (Join-Path $publish '相关文档\api文档.md')
}

Write-Host '==> 压缩 zip'
New-Item -ItemType Directory -Force -Path $zipDir | Out-Null
Compress-Archive -Path (Join-Path $publish '*') -DestinationPath $zipOut -Force

Write-Host "==> 完成: $zipOut"
Write-Host "解压后目录布局：VoiceTableAssist.exe + models/{raner,embedding,asr} + sherpa-onnx/{exe,hr} + wwwroot/ + appsettings.json"
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

# GTCRN 降噪模型（可选，522KB）：工厂噪声场景 Denoise.Enabled=true 时必需
$gtcrn = Join-Path $publish 'models\asr\gtcrn_simple.onnx'
if (Test-Path $gtcrn) {
    $kb = [math]::Round((Get-Item $gtcrn).Length / 1KB)
    Write-Host "==> GTCRN 降噪模型已随包：models\asr\gtcrn_simple.onnx ($kb KB)"
    Write-Host '    默认启用：appsettings.json <- AsrProvider.Denoise.Enabled = true'
} else {
    Write-Warning 'GTCRN 模型未随包：models\asr\gtcrn_simple.onnx'
    Write-Warning '  工厂噪声场景建议补上：https://github.com/k2-fsa/sherpa-onnx/releases/download/speech-enhancement-models/gtcrn_simple.onnx'
    Write-Warning '  缺失时启动自动降级关闭降噪（Denoise.Enabled=true 也仅 WARN 不阻断）'
}

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
# 一键部署入口：双击即跑 deploy-check.ps1 -Selftest（含 VC++ 运行时缺失检测与静默安装）
if (Test-Path (Join-Path $project 'install.bat')) {
    Copy-Item -Force (Join-Path $project 'install.bat') (Join-Path $publish 'install.bat')
}
# HTTPS 证书生成脚本：install.bat 在 certs\gateway.pfx 或 wwwroot\ca.crt 缺失时自动调它，
# 目标机若无这两个文件、又不带 make-cert，则 HTTPS 自愈会失败。所以必须随包带。
if (Test-Path (Join-Path $project 'make-cert.bat')) {
    Copy-Item -Force (Join-Path $project 'make-cert.bat') (Join-Path $publish 'make-cert.bat')
}
if (Test-Path (Join-Path $project 'make-cert.ps1')) {
    Copy-Item -Force (Join-Path $project 'make-cert.ps1') (Join-Path $publish 'make-cert.ps1')
}
# VC++ Redistributable x64：目标机缺它时 sherpa-onnx exe 会启动失败（0xC0000135 或缺 VCRUNTIME140.dll）。
# 随包带安装包，deploy-check.ps1 检测到缺失时静默安装（/install /quiet /norestart），无需联网。
$vcRedist = Join-Path $project 'vc_redist.x64.exe'
if (Test-Path $vcRedist) {
    Copy-Item -Force $vcRedist (Join-Path $publish 'vc_redist.x64.exe')
    Write-Host '==> 已附带 VC++ Redistributable x64 安装包（约 25MB，目标机缺时静默安装）'
} else {
    Write-Warning '未找到 vc_redist.x64.exe（目标机若缺 VC++ 运行时 sherpa exe 将启动失败）'
}

# 附带安卓 Cordova 壳模板（config.xml + package.json + build.ps1 + 已同步的 www/）
if (Test-Path (Join-Path $project 'cordova')) {
    Copy-Item -Recurse -Force (Join-Path $project 'cordova') (Join-Path $publish 'cordova')
}

# 附带 HTTPS 证书（可选）：存在即启用平板浏览器直访的 https://15433。
# make-cert.bat 生成 certs/{ca.crt,gateway.pfx}；私钥不进 git，打包机生成后随包分发。
$certsSrc = Join-Path $project 'certs'
if (Test-Path (Join-Path $certsSrc 'gateway.pfx')) {
    Copy-Item -Recurse -Force $certsSrc (Join-Path $publish 'certs')
    Write-Host '==> 已附带 certs/（HTTPS 平板直访已启用）'
} else {
    Write-Warning '未找到 certs\gateway.pfx - HTTPS 已禁用。平板浏览器直访需先运行 make-cert.bat。'
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
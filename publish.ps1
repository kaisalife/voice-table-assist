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

# sherpa 已改为进程内识别（P/Invoke c-api.dll）：不再需要 online-websocket-server.exe，
# 即使本机 models/ 里还留着旧文件也不入包，保证发布包与安卓方案一致（无 server exe）。
Get-ChildItem (Join-Path $publish 'models') -Recurse -File -Filter 'sherpa-onnx-online-websocket-server.exe' |
    ForEach-Object { Remove-Item -Force $_.FullName; Write-Host "==> 已剔除 server exe: $($_.Name)" }
# 模型原始压缩包（下载原件，如 asr 的 *.zip）运行不需要，不入包（单此一项省 550MB+）
Get-ChildItem (Join-Path $publish 'models') -Recurse -File -Include *.zip, *.tar.bz2, *.tar.gz |
    Remove-Item -Force

# GTCRN 降噪模型（可选，522KB）：工厂噪声场景 Denoise.Enabled=true 时必需
$gtcrn = Join-Path $publish 'models\asr\gtcrn_simple.onnx'
if (Test-Path $gtcrn) {
    $kb = [math]::Round((Get-Item $gtcrn).Length / 1KB)
    Write-Host "==> GTCRN 降噪模型已随包：models\asr\gtcrn_simple.onnx ($kb KB)"
    Write-Host '    工厂噪声场景启用：appsettings.json <- AsrProvider.Denoise.Enabled = true（默认 false）'
} else {
    Write-Warning 'GTCRN 模型未随包：models\asr\gtcrn_simple.onnx'
    Write-Warning '  工厂噪声场景建议补上：https://github.com/k2-fsa/sherpa-onnx/releases/download/speech-enhancement-models/gtcrn_simple.onnx'
    Write-Warning '  缺失时启动自动降级关闭降噪（Denoise.Enabled=true 也仅 WARN 不阻断）'
}

# 交付包不带运行期用户数据（表注册表 registry.json / 各表向量索引）：
# 首次启动服务后由前端初始化自动导入建库，避免旧机器的表状态随包污染新部署
$embTables = Join-Path $publish 'models\embedding\tables'
if (Test-Path $embTables) { Remove-Item -Recurse -Force $embTables }

# sherpa-onnx 原生运行时已归档在 models/sherpa-onnx（随上面 models 拷贝自动带上，NativeDir 指向此处）：
# 识别在网关进程内（P/Invoke，c-api.dll），热词按表随流传入、不读文件——
# 这里预置一个空热词文件仅为运维查看/历史兼容（运行时不依赖它）。
$hrCurrent = Join-Path $publish 'sherpa-onnx\hr\tables\current'
New-Item -ItemType Directory -Force -Path $hrCurrent | Out-Null
if (-not (Test-Path (Join-Path $hrCurrent 'hotwords.txt'))) {
    [System.IO.File]::WriteAllText((Join-Path $hrCurrent 'hotwords.txt'), '', [System.Text.UTF8Encoding]::new($false))
    Write-Host '==> 已预置空热词文件 sherpa-onnx/hr/tables/current/hotwords.txt（仅供查看）'
}

# 剔除"安卓化更新引入、C# 网关用不到"的模型文件（源码 models/ 不动，只清发布副本）：
#   - sherpa-onnx-streaming-zipformer-zh-int8-*：安卓端用的 int8 模型（C# 用 float32 版，精度更高）
#   - silero_vad.onnx：安卓端 VAD（C# 用 sherpa 端点检测，不读它）
#   - models/sherpa-onnx/hr/**：安卓端生成的拼音字典 + 热词目录（字典已随代码走 assets/hr_char_pinyin.txt，
#     热词按表随流传入；运行期只需 exe 目录下的 sherpa-onnx/hr/tables 占位）
$androidOnly = @(
    (Join-Path $publish 'models\asr\sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30'),
    (Join-Path $publish 'models\asr\silero_vad.onnx'),
    (Join-Path $publish 'models\sherpa-onnx\hr')
)
foreach ($p in $androidOnly) {
    if (Test-Path $p) { Remove-Item -Recurse -Force $p; Write-Host "==> 已剔除安卓化模型文件（不让入包）: $($p.Replace($publish + '\', ''))" }
}
Get-ChildItem (Join-Path $publish 'models\asr') -Directory -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -like '*int8*' } |
    ForEach-Object { Remove-Item -Recurse -Force $_.FullName; Write-Host "==> 已剔除安卓 int8 模型目录: $($_.Name)" }

# 附带部署检查脚本与多表自测脚本（临时拉起验证，关掉脚本即停）。
# install.bat 里 `call "%~dp0deploy-check.bat" /SELFTEST` 是随包一键入口，必须两个都在（只带 .ps1 会导致入口 404）。
Copy-Item -Force (Join-Path $project 'deploy-check.ps1') (Join-Path $publish 'deploy-check.ps1')
if (Test-Path (Join-Path $project 'deploy-check.bat')) {
    Copy-Item -Force (Join-Path $project 'deploy-check.bat') (Join-Path $publish 'deploy-check.bat')
} else {
    Write-Warning '未找到 deploy-check.bat —— install.bat 的一键入口会找不到它'
}
Copy-Item -Recurse -Force (Join-Path $project 'selftest') (Join-Path $publish 'selftest')
# 一键部署入口：双击即跑 deploy-check.ps1 -Selftest（含 VC++ 运行时缺失检测与静默安装）
if (Test-Path (Join-Path $project 'install.bat')) {
    Copy-Item -Force (Join-Path $project 'install.bat') (Join-Path $publish 'install.bat')
}
# 证书相关（make-cert.*、certs/）**不入包**：平板现在走 Cordova 壳内的 http://localhost 安全上下文，
# 不再需要 HTTPS 证书；如现场确需浏览器直访 https://<网关IP>:15433，再自行在包外放 certs/gateway.pfx。
# VC++ Redistributable x64：目标机缺它时 sherpa 原生库加载失败（缺 VCRUNTIME140.dll）。
# 随包带安装包，deploy-check.ps1 检测到缺失时静默安装（/install /quiet /norestart），无需联网。
$vcRedist = Join-Path $project 'vc_redist.x64.exe'
if (Test-Path $vcRedist) {
    Copy-Item -Force $vcRedist (Join-Path $publish 'vc_redist.x64.exe')
    Write-Host '==> 已附带 VC++ Redistributable x64 安装包（约 25MB，目标机缺时静默安装）'
} else {
    Write-Warning '未找到 vc_redist.x64.exe（目标机若缺 VC++ 运行时 sherpa exe 将启动失败）'
}

# 附带安卓 Cordova 壳模板（只带壳源：config.xml + package.json + build.ps1 + 已同步的 www/）
# 注意：绝不能整目录递归拷 cordova/ —— 里面 platforms/（gradle 构建缓存，几十万小文件 + 超长路径）
# 会把发布包撑爆并触发 Windows 路径长度限制；node_modules/ 同理。
$cordovaSrc = Join-Path $project 'cordova'
if (Test-Path $cordovaSrc) {
    $cordovaDst = Join-Path $publish 'cordova'
    if (Test-Path $cordovaDst) { Remove-Item -Recurse -Force $cordovaDst }
    New-Item -ItemType Directory -Force -Path $cordovaDst | Out-Null
    foreach ($item in 'config.xml', 'package.json', 'package-lock.json', 'build.ps1', 'www', 'hooks', 'plugins') {
        $src = Join-Path $cordovaSrc $item
        if (Test-Path $src) { Copy-Item -Recurse -Force $src (Join-Path $cordovaDst $item) }
    }
    Write-Host '==> 已附带 cordova 壳模板（config.xml/package.json/build.ps1/www，不含 platforms/node_modules）'
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

# ---- 交付前完整性自检：少文件/多安卓文件都当场 FAIL，避免带着问题发到现场 ----
Write-Host '==> 发布包完整性自检'
$required = @(
    'VoiceTableAssist.exe', 'VoiceTableAssist.dll', 'appsettings.json', 'web.config',
    'assets\hr_char_pinyin.txt',                                   # 读音吸附字典（随代码发布）
    'wwwroot\index.html', 'wwwroot\voice-mic.js', 'wwwroot\audio-capture-worklet.js',
    'models\asr\sherpa-onnx-streaming-zipformer-zh-2025-06-30\encoder.onnx',
    'models\asr\sherpa-onnx-streaming-zipformer-zh-2025-06-30\decoder.onnx',
    'models\asr\sherpa-onnx-streaming-zipformer-zh-2025-06-30\joiner.onnx',
    'models\asr\sherpa-onnx-streaming-zipformer-zh-2025-06-30\tokens.txt',
    'models\raner\model.onnx',
    'models\embedding\model_quantized.onnx',
    'models\sherpa-onnx\sherpa-onnx-c-api.dll',                    # 进程内识别（P/Invoke）
    'models\sherpa-onnx\onnxruntime.dll',
    'selftest\selftest.ps1', 'deploy-check.ps1', 'deploy-check.bat', 'install.bat',
    '相关文档\部署文档.md', '相关文档\用户使用指南.md', '相关文档\api文档.md'
)
$missing = @()
foreach ($rel in $required) { if (-not (Test-Path (Join-Path $publish $rel))) { $missing += $rel } }
if ($missing.Count -gt 0) {
    Write-Host 'FAIL  发布包缺少以下文件：'
    $missing | ForEach-Object { Write-Host "        $_" }
    throw "发布包不完整（缺 $($missing.Count) 项），已中止"
}

# 安卓化更新引入的文件不得入包（models 里只允许 C# 运行期需要的资产）
$forbidden = @(
    'models\asr\silero_vad.onnx',
    'models\sherpa-onnx\hr\hr_char_pinyin.txt'
)
foreach ($rel in $forbidden) { if (Test-Path (Join-Path $publish $rel)) { throw "安卓化模型文件混入发布包：$rel" } }
if (Get-ChildItem (Join-Path $publish 'models\asr') -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -like '*int8*' }) {
    throw '安卓 int8 模型目录混入发布包：models\asr\*int8*'
}
Write-Host "OK    完整性自检通过（必需 $($required.Count) 项齐全；无安卓化模型文件）"

Write-Host "==> 完成: $zipOut"
Write-Host "解压后目录布局：VoiceTableAssist.exe + models/{raner,embedding,asr(float32)} + sherpa-onnx/{c-api.dll,onnxruntime.dll} + assets/拼音字典 + wwwroot/ + appsettings.json"


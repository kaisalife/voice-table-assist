# 安卓平板部署 + 验收（一鍵）
#
# 前置：
#   1. 平板开「开发者选项 → USB 调试」，USB 连接本机并允许调试
#   2. adb 可用（默认取 $env:ANDROID_HOME/platform-tools/adb.exe，或本机 MuMu 的 adb）
#   3. 模型资产已生成：相关文档\安卓化方案\android\assets\models（scripts\copy_models.ps1）
#
# 用法（在 VoiceTableAssist 目录）：
#   powershell -ExecutionPolicy Bypass -File 相关文档\安卓化方案\scripts\deploy_android.ps1
#   powershell ... -File ...\deploy_android.ps1 -Device 192.168.1.20:5555   # 无线调试
param(
    [string]$Device = "",
    [string]$DistDir = "相关文档\安卓化方案\dist\arm64-v8a",
    [string]$ModelsDir = "相关文档\安卓化方案\android\assets\models",
    [switch]$SkipModels
)
$ErrorActionPreference = "Stop"

# ---- adb 定位 ----
$adb = @(
    "$env:ANDROID_HOME\platform-tools\adb.exe",
    "D:\Android\Sdk\platform-tools\adb.exe",
    "D:\Program Files\Netease\MuMu\nx_main\adb.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $adb) { throw "未找到 adb.exe，请设置 ANDROID_HOME 或改脚本内路径" }
Write-Host "adb: $adb"

if ($Device) { & $adb connect $Device | Out-Null; & $adb -s $Device wait-for-device }
$dev = if ($Device) { $Device } else { (& $adb devices | Select-String "device$" | Select-Object -First 1) -replace "\s+device.*","" -replace "^\s*","" }
if (-not $dev) { throw "未检测到设备。请确认 USB 调试已授权，或用 -Device <ip:port> 指定无线调试" }
Write-Host "设备: $dev"

$t = "/data/local/tmp/vta"
& $adb -s $dev shell "mkdir -p $t/lib $t/models"
& $adb -s $dev push "$DistDir\vta-service" "$t/vta-service"
& $adb -s $dev push "$DistDir\libonnxruntime.so" "$t/lib/libonnxruntime.so"
& $adb -s $dev push "$ModelsDir" "$t/models_sync"
& $adb -s $dev shell "cd $t && cp -rn models_sync/* models/ && cp models_sync/vta.json ./vta.json && rm -rf models_sync && chmod +x vta-service"
& $adb -s $dev push "相关文档\安卓化方案\dist\selftest_16k.f32" "/data/local/tmp/selftest_16k.f32"

Write-Host "`n=== 运行数值自检（TTS: 硬度一号是十七点八四二号测量值六十）==="
& $adb -s $dev logcat -c
& $adb -s $dev shell "cd $t && LD_LIBRARY_PATH=$t/lib ./vta-service --selftest /data/local/tmp/selftest_16k.f32 --data-dir $t 2>&1"
Write-Host "`n=== 关键日志 ==="
& $adb -s $dev logcat -d -s VTA | Select-String "MODELS|TABLES|ASR\]|SESSION|SUBMIT|cell|done|SELFTEST"
Write-Host @"

判定标准：
  - [MODELS] 引擎按需加载完成      → RaNER/嵌入加载成功
  - [ASR] partial: 硬度一号是十七点八四...（字级正确）→ ASR int8 + 热词生效
  - [SUBMIT] -> 2 个三元组
  - cell: row=6 col=1 value=17.84 raw="十七点八四"     → 端到端正确
  - exit code 0 / done: N cells

若 partial 全空或乱码 → 查 ORT 版本与 SoC 组合；若 cell 为 0 → 看 SUBMIT 日志定位。
"@

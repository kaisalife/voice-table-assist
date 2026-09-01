# ============================================================================
# 部署效果检查（"临时拉起，关掉即停"）
#
# 运行本脚本会拉起 VoiceTableAssist 服务并做就绪自检 + 打一张测试页结果，
# 然后**保持服务运行**；当你回车 / 关闭本窗口 / 按 Ctrl+C 时，脚本会停止服务。
# 也就是说：默认(不运行脚本)服务不启动；只有部署时用本脚本临时验证效果。
#
# 用法（在解压后的部署目录，即含 VoiceTableAssist.exe 的目录）：
#   powershell -ExecutionPolicy Bypass -File .\deploy-check.ps1
#   可选：
#     -Port 15232             # 覆盖端口（需与 appsettings.json 的 Urls 一致）
#     -Selftest               # 就绪后再跑一遍多表热切换自测（无语音，只调接口）
#     -KeepAliveSeconds 60    # 自动退出模式：N 秒后自动停（用于 CI/无人值守）
#   退出码 0=检查通过；非 0=失败。
# ============================================================================
param(
    [int]$Port = 15232,
    [switch]$Selftest,
    [int]$KeepAliveSeconds = 0
)
$ErrorActionPreference = 'Stop'

$exe = Join-Path $PSScriptRoot 'VoiceTableAssist.exe'
if (-not (Test-Path $exe)) { Write-Error "未找到 $exe（请在本脚本所在的分发目录运行）" }
$base = "http://127.0.0.1:$Port"
# 注意：用 127.0.0.1 而不是 localhost——Windows 上 localhost 先解析到 IPv6 ::1，
# 而 Kestrel 只绑定 IPv4 时 PowerShell 的 Invoke-RestMethod 会因回退等待而超时误判。

# ---- -1) 目标机运行环境前置处理：VC++ 运行时缺失检测 + 静默安装；解 MOTW 锁 ----
# sherpa-onnx-online-websocket-server.exe 依赖 VC++ 2015-2022 x64 运行时（含较新的
# VCRUNTIME140_1.dll / MSVCP140_1.dll）；目标机缺它或装旧版时启动报 0xC000007B。
# 本节随包带 vc_redist.x64.exe，检测到任一关键 DLL 缺失即静默安装（覆盖旧版）。
# 同时对所有 exe/dll 跑 Unblock-File，解除下载/拷贝产生的 Mark-of-the-Web，避免 SmartScreen 拦截。
function Test-VcRuntime {
    # 必须齐全的 5 个 DLL（VC++ 2015-2022 较新版本才有 _1 后缀的）
    $need = @('VCRUNTIME140.dll','VCRUNTIME140_1.dll','MSVCP140.dll','MSVCP140_1.dll')
    foreach ($d in $need) {
        if (-not (Test-Path "$env:SystemRoot\System32\$d")) { return $false }
    }
    return $true
}
Write-Host "==> 检查目标机运行环境 ..."
if (-not (Test-VcRuntime)) {
    $vcExe = Join-Path $PSScriptRoot 'vc_redist.x64.exe'
    if (Test-Path $vcExe) {
        Write-Host "     VC++ 运行时缺失或版本过旧，开始静默安装（约 10 秒，覆盖旧版）..."
        $p = Start-Process -FilePath $vcExe -ArgumentList '/install','/quiet','/norestart' -Wait -PassThru
        if ($p.ExitCode -eq 0 -or $p.ExitCode -eq 1638 -or $p.ExitCode -eq 3010) {
            Write-Host "OK    VC++ 运行时已安装（ExitCode=$($p.ExitCode)）"
            # 装完再验一次；若仍缺，明确报出哪个 DLL 缺，方便定位
            if (-not (Test-VcRuntime)) {
                $missing = @('VCRUNTIME140.dll','VCRUNTIME140_1.dll','MSVCP140.dll','MSVCP140_1.dll') |
                    Where-Object { -not (Test-Path "$env:SystemRoot\System32\$_") }
                Write-Host "FAIL  安装后仍缺 DLL: $($missing -join ', ')"
                Write-Host "      请手工双击 vc_redist.x64.exe 安装，或重启目标机后再试"
                exit 1
            }
        } else {
            Write-Host "FAIL  VC++ 运行时安装失败（ExitCode=$($p.ExitCode)）；请手工双击 vc_redist.x64.exe 安装后重试"
            exit 1
        }
    } else {
        Write-Host "FAIL  目标机缺 VC++ 运行时，且部署包内未带 vc_redist.x64.exe"
        Write-Host "      请到 https://aka.ms/vs/17/release/vc_redist.x64.exe 下载安装后重试"
        exit 1
    }
} else {
    Write-Host "OK    VC++ 运行时已就绪（含 _1 后缀的新版 DLL）"
}
# 解 MOTW：从网络下载或拷贝来的 exe/dll 会被标 MOTW，双击触发 SmartScreen；这里统一解锁。
# -ErrorAction SilentlyContinue：无 MOTW 的文件会报"未标记"，忽略即可。
Get-ChildItem $PSScriptRoot -Recurse -Include *.exe,*.dll -File |
    ForEach-Object { Unblock-File -Path $_.FullName -ErrorAction SilentlyContinue }
Write-Host "OK    已解除 SmartScreen 锁定（Unblock-File）"

# ---- -0.5) sherpa exe 直接启动测试：拿到明确 ExitCode，不再笼统报 0xC000007B ----
# 用 --help 让 sherpa exe 打印帮助后退出（不监听端口、不阻塞）；ExitCode 0 = 依赖齐全。
# 失败时列出所有关键依赖 DLL 的存在状态，方便一眼定位缺哪个。
Write-Host "==> 验证 sherpa-onnx 原生库依赖 ..."
$sherpaExe = Join-Path $PSScriptRoot 'models\sherpa-onnx\sherpa-onnx-online-websocket-server.exe'
$sherpaTest = Start-Process -FilePath $sherpaExe -ArgumentList '--help' -Wait -PassThru -NoNewWindow
if ($sherpaTest.ExitCode -eq 0) {
    Write-Host "OK    sherpa-onnx exe 依赖齐全（ExitCode=0）"
} else {
    Write-Host "FAIL  sherpa-onnx exe 启动失败（ExitCode=$($sherpaTest.ExitCode) = 0x$('{0:X}' -f ([uint32]$sherpaTest.ExitCode))）"
    Write-Host "      关键依赖 DLL 状态："
    $deps = @('dxgi.dll','VCRUNTIME140.dll','VCRUNTIME140_1.dll','MSVCP140.dll','MSVCP140_1.dll','dbghelp.dll','SETUPAPI.dll','WS2_32.dll','MSWSOCK.dll')
    foreach ($d in $deps) {
        $ok = Test-Path "$env:SystemRoot\System32\$d"
        Write-Host ("        {0,-25} {1}" -f $d, $(if ($ok) { 'OK' } else { '缺失' }))
    }
    Write-Host "      处理建议：若 VC++ 相关 DLL 缺失，重装 vc_redist.x64.exe；若 dxgi.dll 缺失，装 DirectX 修复工具"
    exit 1
}

# ---- 0) 部署完整性自检：关键文件缺失提前 FAIL，避免拉起后才暴露部署遗漏 ----
Write-Host "==> 部署完整性自检 ..."
$script:fail = 0
function Check-File([string]$rel, [string]$desc) {
    if (Test-Path (Join-Path $PSScriptRoot $rel)) { Write-Host "OK    $desc" }
    else { Write-Host "FAIL  缺少 ${desc}: $rel"; $script:fail++ }
}
Check-File 'appsettings.json' '服务配置 appsettings.json'
Check-File 'wwwroot\index.html' '测试前端 wwwroot/index.html'
try {
    $cfg = Get-Content (Join-Path $PSScriptRoot 'appsettings.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    foreach ($m in @($cfg.SherpaServer.Encoder, $cfg.SherpaServer.Decoder, $cfg.SherpaServer.Joiner, $cfg.SherpaServer.Tokens)) {
        Check-File $m "ASR 模型 $m"
    }
    # sherpa exe + 同目录原生 DLL（路径随 appsettings SherpaServer:ExePath）
    $exeRel = $cfg.SherpaServer.ExePath
    Check-File $exeRel 'sherpa-onnx 流式识别服务 exe'
    $exeDir = Split-Path (Join-Path $PSScriptRoot $exeRel) -Parent
    foreach ($dll in 'onnxruntime.dll', 'onnxruntime_providers_shared.dll', 'sherpa-onnx-c-api.dll', 'sherpa-onnx-cxx-api.dll') {
        if (Test-Path (Join-Path $exeDir $dll)) { Write-Host "OK    sherpa 运行时 DLL $dll" }
        else { Write-Host "FAIL  缺少 sherpa 运行时 DLL: $exeDir\$dll"; $script:fail++ }
    }
    Check-File (Join-Path 'models' 'raner') 'RaNER 模型目录 models/raner'
    Check-File (Join-Path 'models' 'embedding') '嵌入模型目录 models/embedding'
    if (Test-Path (Join-Path $PSScriptRoot $cfg.SherpaServer.HotwordsFile)) {
        Write-Host "OK    sherpa 热词文件 $($cfg.SherpaServer.HotwordsFile)"
    } else {
        Write-Host "WARN  热词文件暂缺（服务启动会自动创建，导入表后自动聚合填充）: $($cfg.SherpaServer.HotwordsFile)"
    }
} catch { Write-Host "WARN  appsettings 解析失败，跳过模型路径检查: $($_.Exception.Message)" }
if ($script:fail -gt 0) {
    Write-Host "FAIL  部署缺少 $script:fail 个关键文件，请补齐后重新验证。"
    exit 1
}

# ---- 1) 拉起服务（子进程 + 就绪轮询）----
# 若端口已被旧实例占用，本次新启动会绑定失败；必须先排除，否则下面的就绪
# 检查会误判成"新实例已就绪"，而停止阶段只杀得到新进程。
try {
    Invoke-RestMethod -Uri "$base/api/health" -TimeoutSec 1 | Out-Null
    Write-Host "FAIL  端口 $Port 已被占用（已有服务在运行）。请先停止旧实例再执行本脚本。"
    exit 1
} catch { }

Write-Host "==> 启动 VoiceTableAssist ..."
$proc = Start-Process -FilePath $exe -WorkingDirectory $PSScriptRoot -PassThru
Write-Host "==> 等待就绪 ($base/api/health) ..."
$ready = $false
for ($i = 0; $i -lt 60; $i++) {
    Start-Sleep -Seconds 1
    try {
        $h = Invoke-RestMethod -Uri "$base/api/health" -TimeoutSec 2
        $ready = $true; break
    } catch { }
}
if (-not $ready) {
    Write-Host "FAIL  服务未就绪（$base/api/health）"
    if (-not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    Get-Process -Name 'sherpa-onnx-online-websocket-server' -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue
    Write-Host "FAIL  已停止服务及 sherpa-onnx 子进程"; exit 1
}

Write-Host "OK    服务就绪 -> activeTable=$($h.activeTable), provider=$($h.provider)"
try {
    $pg = Invoke-WebRequest -Uri "$base/" -TimeoutSec 5 -UseBasicParsing
    Write-Host "OK    测试页  GET /  => HTTP $([int]$pg.StatusCode)  url: $base/"
} catch {
    Write-Host "WARN  测试页访问失败: $($_.Exception.Message)"
}

# ---- 2/3) 自测 + 保持运行，整体包进 try/finally ----
# PowerShell 中 exit / Ctrl+C / 异常都会先执行 finally，
# 确保任何路径退出都不残留 VoiceTableAssist 和 sherpa-onnx 进程。
try {
    # ---- 2) （可选）多表自测（无语音，只调 HTTP 接口）----
    if ($Selftest) {
        $script:selftestRan = $true   # 无论自测成败，结束后都清理导入的数据
        Write-Host "==> 运行多表自测（http 节：导入/热切换/NER） ..."
        & (Join-Path $PSScriptRoot 'selftest\selftest.ps1') -Base $base -Only http
        if ($LASTEXITCODE -ne 0) { Write-Host "FAIL  自测未全通过"; exit 1 }
    }

    # ---- 3) 保持服务运行；关掉脚本即停 ----
    Write-Host ""
    Write-Host "=============================================================="
    Write-Host "  服务已在运行："
    Write-Host "    测试页   $base/"
    Write-Host "    健康     $base/api/health"
    Write-Host "  关闭本脚本(回车 / Ctrl+C / 关窗口) 即停止服务。"
    Write-Host "=============================================================="
    if ($KeepAliveSeconds -gt 0) {
        Start-Sleep -Seconds $KeepAliveSeconds
    } else {
        Read-Host "按回车停止服务并退出" | Out-Null
    }
} finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -ErrorAction SilentlyContinue }
    # 连带停掉其子进程 sherpa-onnx
    Get-Process -Name 'sherpa-onnx-online-websocket-server' -ErrorAction SilentlyContinue |
        Stop-Process -Force -ErrorAction SilentlyContinue

    # 还原干净：自测导入的数据表（表注册表/向量索引）与聚合热词全部删除，恢复出厂状态。
    # 在服务停止后执行（文件无锁）；下次部署验证或前端初始化会重新导入建库。
    if ($script:selftestRan) {
        Write-Host "==> 清理自测导入的数据表（还原干净）..."
        Remove-Item -Recurse -Force (Join-Path $PSScriptRoot 'models\embedding\tables') -ErrorAction SilentlyContinue
        # 各表语音资源目录（current 之外），聚合热词随之失效
        Get-ChildItem (Join-Path $PSScriptRoot 'sherpa-onnx\hr\tables') -Directory -ErrorAction SilentlyContinue |
            Where-Object Name -ne 'current' |
            Remove-Item -Recurse -Force -ErrorAction SilentlyContinue
        # current 下 default 表的纠错规则也随数据删除，仅保留空热词占位
        Get-ChildItem (Join-Path $PSScriptRoot 'sherpa-onnx\hr\tables\current') -File -ErrorAction SilentlyContinue |
            Where-Object Name -ne 'hotwords.txt' |
            Remove-Item -Force -ErrorAction SilentlyContinue
        $hw = Join-Path $PSScriptRoot 'sherpa-onnx\hr\tables\current\hotwords.txt'
        if (Test-Path $hw) { [System.IO.File]::WriteAllText($hw, '', [System.Text.UTF8Encoding]::new($false)) }
        Write-Host "OK    已还原干净（数据表注册表/向量索引/聚合热词已重置）"
    }

    Write-Host "OK    服务已停止。"
}
exit 0
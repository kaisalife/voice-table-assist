# ============================================================================
# VoiceTableAssist 一体化自测脚本（合并原 hot-switch / ws-ready / asr-e2e 三个脚本）
#
# 三节测试，默认按序全跑：
#   http  多表导入 + 热切换 + /text_to_json、/api/speech/ner 坐标断言（纯 HTTP）
#   ws    用"非活动表"建 WS 连接，测 connect→ready 延迟（握手不得等索引加载）
#   asr   Windows TTS 生成语音（或 -Wav 指定 wav）走真实 WS 链路，验证 partial/final/cells
#
# 用法：
#   powershell -ExecutionPolicy Bypass -File selftest\selftest.ps1                    # 全部三节
#   powershell -ExecutionPolicy Bypass -File selftest\selftest.ps1 -Only http         # 只跑某一节
#   powershell -ExecutionPolicy Bypass -File selftest\selftest.ps1 -Only asr -Wav a.wav
#
# 常用参数：-Base 服务地址；-AsrTable/-AsrText/-Wav 语音用例；-ReadyTimeoutSec ws 节 ready 等待。
# 退出码：0=全部通过（SKIP 不算失败）；非 0=存在失败用例。
# ============================================================================
param(
    [string]$Base = 'http://127.0.0.1:15232',
    [ValidateSet('', 'http', 'ws', 'asr')]
    [string]$Only = '',            # 只跑指定节；缺省全跑
    [string]$AsrTable = 'default', # asr 节目标表（http 节会自动导入 default）
    [string]$AsrText = '硬度一号是五十点零',
    [string]$Wav = '',             # asr 节现成 16k wav；缺省用 Windows TTS 生成
    [int]$AsrTimeoutSec = 90,      # asr 节 ready 总等待
    [int]$ReadyTimeoutSec = 60     # ws 节 ready 等待（懒加载冷启动可能数十秒）
)

$ErrorActionPreference = 'Stop'
$base = $Base.TrimEnd('/')

$script:ok = 0; $script:total = 0
function Check([bool]$pass, [string]$name, [string]$detail) {
    $script:total++
    if ($pass) { $script:ok++; Write-Host "  PASS  $name" }
    else       { Write-Host "  FAIL  $name   $detail" }
}
function Skip([string]$name, [string]$reason) {
    Write-Host "  SKIP  $name   $reason"
}
function Section([string]$name) {
    Write-Host ""
    Write-Host "===== [$name] ====="
}

# 模拟前端调后端：body 必须 UTF-8 字节（PS5.1 默认 Latin-1 中文会变 ????）；
# 导表/嵌入较慢用长超时；池化连接偶发抖动重试一次。
function Frontend-Call {
    param([string]$Path, [object]$Body)
    for ($try = 1; $try -le 3; $try++) {
        try {
            $p = @{ Uri = "$base$Path"; Method = 'Post';
                    ContentType = 'application/json; charset=utf-8';
                    Body = [System.Text.Encoding]::UTF8.GetBytes(($Body | ConvertTo-Json -Depth 8));
                    TimeoutSec = 3000 }
            return Invoke-RestMethod @p
        } catch {
            if ($try -ge 3) { throw }
            Start-Sleep -Milliseconds 800
        }
    }
}

function Get-Health { Invoke-RestMethod -Uri "$base/api/health" -TimeoutSec 5 }
function Get-Tables { (Invoke-RestMethod -Uri "$base/tables" -TimeoutSec 5).tables }

# sherpa ASR 运行时可用性：网关只做转发，真正的识别在 sherpa（6006）。
# bin 目录直接跑服务时通常没有 sherpa-onnx/（publish.ps1 才拷入），ws/asr 两节只能 SKIP。
function Test-AsrReady {
    $c = New-Object System.Net.Sockets.TcpClient
    try {
        $ar = $c.BeginConnect('127.0.0.1', 6006, $null, $null)
        return ($ar.AsyncWaitHandle.WaitOne(800) -and $c.Connected)
    } catch { return $false } finally { $c.Close() }
}

# WS 收一帧（文本返回内容；CLOSE/TIMEOUT/ERROR 返回标记）
function Receive-One($ws, $rbuf, $ms) {
    $cts = [System.Threading.CancellationTokenSource]::new($ms)
    try {
        $r = $ws.ReceiveAsync([ArraySegment[byte]]::new($rbuf), $cts.Token).GetAwaiter().GetResult()
        if ($r.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Close) { return 'CLOSE' }
        if ($r.MessageType -eq [System.Net.WebSockets.WebSocketMessageType]::Text) {
            return [text.encoding]::UTF8.GetString($rbuf, 0, $r.Count)
        }
        return ''
    } catch [OperationCanceledException] { return 'TIMEOUT' }
    catch { return 'ERROR' }
    finally { $cts.Dispose() }
}

# 前置探活：任何一节都要求服务在跑
try { $null = Get-Health }
catch { Write-Error "后端不可达：$base（请先启动 VoiceTableAssist）。"; exit 1 }

$doHttp = ($Only -eq '' -or $Only -eq 'http')
$doWs   = ($Only -eq '' -or $Only -eq 'ws')
$doAsr  = ($Only -eq '' -or $Only -eq 'asr')

# ============================================================================
if ($doHttp) {
    Section 'HTTP 多表导入/热切换/NER'
    try {
        # ---- 表A：default 6x6（行标签大多 2-3 字）----
        $rowsA = @("外径","内径","硬度","光洁度","直线度","圆度") | ForEach-Object -Begin {$i=1} -Process { @{ label=$_; index=$i++ } }
        $ia = Frontend-Call -Path "/import_table" -Body @{ tableName=$null; rows=$rowsA; columnCount=6 }
        Check ($ia.status -eq "ok") "导入表A(default, 6x6)" ($ia | ConvertTo-Json -Compress)

        # ---- 表B：力学性能 3x4（证明无 6x6 硬编码）----
        $rowsB = @("抗拉强度","屈服强度","伸长率") | ForEach-Object -Begin {$i=1} -Process { @{ label=$_; index=$i++ } }
        $ib = Frontend-Call -Path "/import_table" -Body @{ tableName="力学性能"; rows=$rowsB; columnCount=4 }
        Check ($ib.status -eq "ok") "导入表B(力学性能, 3x4)" ($ib | ConvertTo-Json -Compress)

        # ---- 切到表A：硬度一行六列，命中 row=3（硬度是第3个行标签，column=序号）----
        $cellsA = Frontend-Call -Path "/text_to_json" -Body @{ text = "巡检登记，硬度，一号是五十点零，二号是四十九点九九，三号是五十一点五，四号是六十点七五，五号是六十六点七五，六号是四十八点七七"; table = "default" }
        $hitsA = @($cellsA | Where-Object { $_.row -eq 3 -and $null -ne $_.values })
        Check ($hitsA.Count -eq 6) "表A(default) 返回6个单元格(row=3)" ($cellsA | ConvertTo-Json -Compress)
        Check ((Get-Health).activeTable -eq "default") "切到表A后 activeTable=default"

        # ---- 切到表B：命中(1,1,300) ----
        $cellsB = Frontend-Call -Path "/text_to_json" -Body @{ text = "抗拉强度，一号是三百"; table = "力学性能" }
        $hit = @($cellsB | Where-Object { $_.row -eq 1 -and $_.column -eq 1 -and [math]::Round([double]$_.values,2) -eq 300 })
        Check ($hit.Count -eq 1) "表B(力学性能) 命中(1,1,300)" ($cellsB | ConvertTo-Json -Compress)
        Check ((Get-Health).activeTable -eq "力学性能") "切到表B后 activeTable=力学性能"

        # ---- 切回表A：命中(3,1,50) ----
        $cellsA2 = Frontend-Call -Path "/text_to_json" -Body @{ text = "巡检登记，硬度，一号是五十点零"; table = "default" }
        $hit = @($cellsA2 | Where-Object { $_.row -eq 3 -and $_.column -eq 1 -and [math]::Round([double]$_.values,2) -eq 50 })
        Check ($hit.Count -eq 1) "切回表A 命中(3,1,50)" ($cellsA2 | ConvertTo-Json -Compress)
        Check ((Get-Health).activeTable -eq "default") "切回表A后 activeTable=default"

        # ---- /api/speech/ner 表B 命中(2,2,200) ----
        $neo = Frontend-Call -Path "/api/speech/ner" -Body @{ text="屈服强度，二号是二百"; table="力学性能" }
        $hit = @($neo.triples | Where-Object { $_.row -eq 2 -and $_.column -eq 2 -and [math]::Round([double]$_.value,2) -eq 200 })
        Check ($hit.Count -eq 1) "/api/speech/ner 表B 命中(2,2,200)" ($neo | ConvertTo-Json -Compress)
    } catch {
        Check $false "HTTP 节执行异常" "$_"
    }
}

# ============================================================================
if ($doWs) {
    Section 'WS 就绪延迟（握手不等索引加载）'
    try {
        # sherpa 未就绪（bin 目录跑服务常见）时只能跳过：网关无 ASR 后端，ready 帧发不出来
        if (-not (Test-AsrReady)) {
            Skip "WS ready 延迟" "ASR 运行时未就绪（6006 无监听：bin 目录跑服务需 publish.ps1 拷入 sherpa-onnx，或先启动外部 sherpa）"
        } else {
            # 选一个"非当前活动表"的已导入表：优先 http 节导入的 力学性能，其次任意非活动表
            $active = (Get-Health).activeTable
            $tables = @(Get-Tables)
            $cand = $tables | Where-Object { $_.name -ne $active }
            if (-not $cand) {
                Skip "WS ready 延迟" "没有非活动表的已导入表（先跑 http 节或多导一张表）"
            } else {
                $tname = ($cand | Select-Object -First 1).name
                $wsBase = $base -replace '^http', 'ws'
                $uri = "$wsBase/api/speech/asr/stream?table=$([uri]::EscapeDataString($tname))"

                $ws = [System.Net.WebSockets.ClientWebSocket]::new()
                $sw = [System.Diagnostics.Stopwatch]::StartNew()
                $null = $ws.ConnectAsync([uri]$uri, [Threading.CancellationToken]::None).GetAwaiter().GetResult()

                $buffer = New-Object byte[] 65536
                $deadline = [datetime]::UtcNow.AddSeconds($ReadyTimeoutSec)
                $readyMs = -1
                while ([datetime]::UtcNow -lt $deadline) {
                    $m = Receive-One $ws $buffer ([int]([datetime]$deadline - [datetime]::UtcNow).TotalMilliseconds)
                    if ($m -eq 'CLOSE' -or $m -eq 'ERROR') { break }
                    if ($m -eq 'TIMEOUT' -or $m -eq '') { continue }
                    if ($m -match '"type"\s*:\s*"ready"') { $readyMs = $sw.ElapsedMilliseconds; break }
                }

                # 优雅收尾，让服务端会话正常冲刷关闭
                try {
                    $stop = [text.encoding]::UTF8.GetBytes('{"type":"stop"}')
                    $null = $ws.SendAsync([ArraySegment[byte]]::new($stop), [System.Net.WebSockets.WebSocketMessageType]::Text, $true, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
                    Start-Sleep -Milliseconds 500
                } catch { }
                $ws.Dispose()

                if ($readyMs -lt 0) {
                    Check $false "WS ready 延迟(table=$tname)" "${ReadyTimeoutSec}s 内未收到 ready 帧"
                } else {
                    Check ($readyMs -le 1500) "WS ready 延迟(table=$tname) = ${readyMs}ms（应远小于1s，懒加载冷启动除外）"
                    Start-Sleep -Milliseconds 300
                    Write-Host ("  INFO  activeTable now = {0}（后台激活已落定）" -f (Get-Health).activeTable)
                }
            }
        }
    } catch {
        Check $false "WS 节执行异常" "$_"
    }
}

# ============================================================================
if ($doAsr) {
    Section 'ASR 语音端到端（TTS→WS→cells）'
    try {
        # sherpa 未就绪（bin 目录跑服务常见）时只能跳过：网关无 ASR 后端，识别不出文本
        if (-not (Test-AsrReady)) {
            Skip "ASR 端到端" "ASR 运行时未就绪（6006 无监听：bin 目录跑服务需 publish.ps1 拷入 sherpa-onnx，或先启动外部 sherpa）"
        } elseif (-not (@(Get-Tables) | Where-Object { $_.name -eq $AsrTable })) {
            Skip "ASR 端到端" "table [$AsrTable] 未导入（先跑 http 节或改 -AsrTable）"
        } else {
            Add-Type -AssemblyName System.Speech
            # 1. 准备 16k/16bit/mono 语音：优先 -Wav，否则 TTS 生成
            if ($Wav) {
                $wav = (Resolve-Path -LiteralPath $Wav).Path
                Write-Host "  INFO  使用音频文件: $wav ($((Get-Item $wav).Length) bytes)"
            } else {
                $wav = Join-Path $env:TEMP 'selftest-asr.wav'
                $synth = New-Object System.Speech.Synthesis.SpeechSynthesizer
                try { $synth.SelectVoice('Microsoft Huihui Desktop') } catch { }
                $fmt = New-Object System.Speech.AudioFormat.SpeechAudioFormatInfo(16000, [System.Speech.AudioFormat.AudioBitsPerSample]::Sixteen, [System.Speech.AudioFormat.AudioChannel]::Mono)
                $synth.SetOutputToWaveFile($wav, $fmt)
                $synth.Speak($AsrText)
                $synth.Dispose()
                Write-Host "  INFO  TTS 已生成: $wav  内容=[$AsrText]"
            }

            # 2. 读 PCM：定位 data chunk（不假定固定 44 字节头）
            $bytes = [System.IO.File]::ReadAllBytes($wav)
            $dataOff = -1; $dataLen = 0
            for ($i = 0; $i -lt $bytes.Length - 8; $i++) {
                if ($bytes[$i] -eq 0x64 -and $bytes[$i+1] -eq 0x61 -and $bytes[$i+2] -eq 0x74 -and $bytes[$i+3] -eq 0x61) {
                    $dataLen = [BitConverter]::ToInt32($bytes, $i + 4)
                    $dataOff = $i + 8
                    break
                }
            }
            if ($dataOff -lt 0 -or $dataLen -le 0 -or ($dataOff + $dataLen) -gt $bytes.Length) {
                Check $false "ASR 音频解析" "无法解析 wav data chunk (off=$dataOff len=$dataLen total=$($bytes.Length))"
            } else {
                # 3. 网关期望浏览器上行 float32 PCM，这里把 int16 转成 float32
                $sampleCount = [int]($dataLen / 2)
                $floatBytes = New-Object byte[] ($sampleCount * 4)
                for ($i = 0; $i -lt $sampleCount; $i++) {
                    $f = [BitConverter]::ToInt16($bytes, $dataOff + $i * 2) / 32768.0
                    [Array]::Copy([BitConverter]::GetBytes([single]$f), 0, $floatBytes, $i * 4, 4)
                }

                # 4. 连 WS，等 ready（懒加载冷启动可能数十秒）
                $wsBase = $base -replace '^http', 'ws'
                $uri = "$wsBase/api/speech/asr/stream?table=$([uri]::EscapeDataString($AsrTable))"
                $ws = [System.Net.WebSockets.ClientWebSocket]::new()
                $null = $ws.ConnectAsync([uri]$uri, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
                Write-Host "  INFO  WS 已连接，等待 ready（冷启动可能较久）..."

                $rbuf = New-Object byte[] 65536
                $deadline = [datetime]::UtcNow.AddSeconds($AsrTimeoutSec)
                $ready = $false
                while ([datetime]::UtcNow -lt $deadline -and -not $ready) {
                    $m = Receive-One $ws $rbuf 8000
                    if ($m -eq 'CLOSE' -or $m -eq 'ERROR') { break }
                    if ($m -eq 'TIMEOUT') { Write-Host '        ...仍在等待服务端（懒加载中）'; continue }
                    if ($m -eq '') { continue }
                    Write-Host "RECV> $m"
                    if ($m -match '"type"\s*:\s*"ready"') { $ready = $true; break }
                }

                if (-not $ready) {
                    Check $false "ASR 端到端" "未收到 ready 帧"
                } else {
                    # 5. 分块发送 float32 PCM（模拟流式，每块 ~64ms）
                    $chunk = 1024 * 4
                    $offset = 0
                    while ($offset -lt $floatBytes.Length) {
                        $n = [Math]::Min($chunk, $floatBytes.Length - $offset)
                        $seg = New-Object byte[] $n
                        [Array]::Copy($floatBytes, $offset, $seg, 0, $n)
                        $null = $ws.SendAsync([ArraySegment[byte]]::new($seg), [System.Net.WebSockets.WebSocketMessageType]::Binary, $true, [Threading.CancellationToken]::None).GetAwaiter().GetResult()
                        $offset += $n
                        Start-Sleep -Milliseconds 60
                    }
                    Write-Host "  INFO  已发送 $sampleCount 个采样（$([Math]::Round($sampleCount/16000,2))s）"

                    # 6. 发 stop，等静默提交流水线返回 cells
                    $stop = [text.encoding]::UTF8.GetBytes('{"type":"stop"}')
                    $null = $ws.SendAsync([ArraySegment[byte]]::new($stop), [System.Net.WebSockets.WebSocketMessageType]::Text, $true, [Threading.CancellationToken]::None).GetAwaiter().GetResult()

                    $deadline = [datetime]::UtcNow.AddSeconds(25)
                    $gotFinal = $false; $gotCells = $false
                    while ([datetime]::UtcNow -lt $deadline) {
                        $m = Receive-One $ws $rbuf 3000
                        if ($m -eq 'CLOSE' -or $m -eq 'ERROR') { break }
                        if ($m -eq 'TIMEOUT' -or $m -eq '') { continue }
                        Write-Host "RECV> $m"
                        if ($m -match '"type"\s*:\s*"final"') { $gotFinal = $true }
                        if ($m -match '"type"\s*:\s*"cells"') { $gotCells = $true; break }
                    }

                    if ($gotCells) { Check $true "ASR 端到端收到 cells（识别+NER 解析成功）" }
                    elseif ($gotFinal) { Check $false "ASR 端到端" "有 final 文本但无 cells（NER 未命中，文本或表内容见上方 RECV）" }
                    else { Check $false "ASR 端到端" "既无 final 也无 cells（ASR 未识别出文本）" }
                }
                $ws.Dispose()
            }
        }
    } catch {
        Check $false "ASR 节执行异常" "$_"
    }
}

# ============================================================================
Write-Host ""
Write-Host "[TEST] 通过 $script:ok/$script:total"
if ($script:ok -eq $script:total) { Write-Host "[TEST] 全部通过"; exit 0 }
Write-Host "[TEST] 存在失败用例"; exit 1

# 拷贝模型与数据资源到 android/assets/models（方案 §4.1）
# 用法（在 VoiceTableAssist 目录下）：
#   powershell -File 相关文档\安卓化方案\scripts\copy_models.ps1
$ErrorActionPreference = "Stop"

$src = Resolve-Path "models"
$dst = Join-Path (Resolve-Path "相关文档\安卓化方案") "android\assets\models"

function Copy-ModelFile($rel) {
    $from = Join-Path $src $rel
    $to = Join-Path $dst $rel
    if (-not (Test-Path $from)) {
        Write-Warning "缺文件（跳过）：$rel"
        return
    }
    New-Item -ItemType Directory -Force -Path (Split-Path $to) | Out-Null
    Copy-Item $from $to -Force
    Write-Host "copied $rel"
}

# ASR：官方 int8 优先（154MB，方案 §13.1）；无 int8 才拷 fp32 变体（避免 assets 翻倍）
$asrDir = Get-ChildItem (Join-Path $src "asr") -Directory -Filter "sherpa-onnx-streaming-zipformer-zh-*" -ErrorAction SilentlyContinue
$int8Dir = $asrDir | Where-Object { Test-Path (Join-Path $_.FullName "encoder.int8.onnx") } | Select-Object -First 1
if ($int8Dir) {
    $rel = "asr\" + $int8Dir.Name
    foreach ($f in @("encoder.int8.onnx", "decoder.onnx", "joiner.int8.onnx", "tokens.txt")) {
        Copy-ModelFile ("$rel\$f")
    }
    # 清掉 assets 里早前拷入的 fp32 变体（省 597MB）
    foreach ($d in $asrDir) {
        if (-not (Test-Path (Join-Path $d.FullName "encoder.int8.onnx"))) {
            $stale = Join-Path $dst ("asr\" + $d.Name)
            if (Test-Path $stale) {
                Remove-Item $stale -Recurse -Force
                Write-Host "removed stale fp32 variant: asr\$($d.Name)"
            }
        }
    }
} else {
    foreach ($d in $asrDir) {
        $rel = "asr\" + $d.Name
        foreach ($f in @("encoder.onnx", "decoder.onnx", "joiner.onnx", "tokens.txt")) {
            Copy-ModelFile ("$rel\$f")
        }
    }
}
# test_wavs 一并拷入（§8.2 数值自检用）
if ($int8Dir -and (Test-Path (Join-Path $int8Dir.FullName "test_wavs"))) {
    $wavsDst = Join-Path $dst ("asr\" + $int8Dir.Name + "\test_wavs")
    New-Item -ItemType Directory -Force -Path $wavsDst | Out-Null
    Copy-Item (Join-Path $int8Dir.FullName "test_wavs\*") $wavsDst -Force
    Write-Host "copied test_wavs（数值自检用）"
}
Copy-ModelFile "asr\gtcrn_simple.onnx"
Copy-ModelFile "asr\silero_vad.onnx"

# RaNER：优先 reduce_range 产物（规避 ORT x86 u8xs8 饱和缺陷，见 export-int8-reduced/quantization_info.json）；
# 无则回退 export-int8；再无则回退 fp32。
$ranerReduced = Join-Path $src "raner\export-int8-reduced"
$ranerInt8 = Join-Path $src "raner\export-int8"
if (Test-Path (Join-Path $ranerReduced "model.onnx")) {
    $ranerSrc = $ranerReduced
    Write-Host "RaNER: 使用 export-int8-reduced（98MB，已验证 x86/ARM 双平台可用）"
} elseif (Test-Path (Join-Path $ranerInt8 "model.onnx")) {
    $ranerSrc = $ranerInt8
    Write-Warning "RaNER: 回退 export-int8（未 reduce_range；ARM64 可用，x86 会饱和算错）"
} else {
    $ranerSrc = $null
}
if ($null -ne $ranerSrc) {
    foreach ($f in @("model.onnx", "vocab.txt", "config.json",
                     "crf_transitions.npy", "crf_start_transitions.npy", "crf_end_transitions.npy")) {
        $from = Join-Path $ranerSrc $f
        if (Test-Path $from) {
            $to = Join-Path $dst "raner\$f"
            New-Item -ItemType Directory -Force -Path (Split-Path $to) | Out-Null
            Copy-Item $from $to -Force
        }
    }
    Write-Host "RaNER 就位: $ranerSrc"
} else {
    Write-Warning "无量化产物，回退 fp32 raner/"
    Copy-ModelFile "raner\model.onnx"
    Copy-ModelFile "raner\vocab.txt"
    Copy-ModelFile "raner\crf_transitions.json"
}

# 嵌入
Copy-ModelFile "embedding\model_quantized.onnx"
Copy-ModelFile "embedding\tokenizer.json"

# 表向量库（VTX1）——【待定：打包哪些表】默认全部拷贝
$tables = Join-Path $src "embedding\tables"
if (Test-Path $tables) {
    Get-ChildItem $tables -Directory | ForEach-Object {
        $bin = Join-Path $_.FullName "cell_index.bin"
        if (Test-Path $bin) {
            Copy-ModelFile ("embedding\tables\" + $_.Name + "\cell_index.bin")
        }
    }
    Copy-ModelFile "embedding\tables\registry.json"
}

# HR 拼音表（表内读音对齐用）
Copy-ModelFile "sherpa-onnx\hr\hr_char_pinyin.txt"

Write-Host "done -> $dst"

# ---- 生成 vta.json：按实际拷入的文件自适应（ASR int8 优先，fp32 兜底）----
$asrRel = $null; $enc = $null; $joi = $null
foreach ($d in $asrDir) {
    if (Test-Path (Join-Path $dst "asr\$($d.Name)\encoder.int8.onnx")) {
        $asrRel = "models/asr/$($d.Name)"; $enc = "encoder.int8.onnx"; $joi = "joiner.int8.onnx"; break
    }
}
if ($null -eq $asrRel) {
    foreach ($d in $asrDir) {
        if (Test-Path (Join-Path $dst "asr\$($d.Name)\encoder.onnx")) {
            $asrRel = "models/asr/$($d.Name)"; $enc = "encoder.onnx"; $joi = "joiner.onnx"; break
        }
    }
}
$hasRanerInt8 = Test-Path (Join-Path $dst "raner\crf_transitions.npy")
$vta = [ordered]@{}
if ($asrRel) {
    $vta["asrModelDir"] = $asrRel
    $vta["asrEncoder"] = $enc
    $vta["asrJoiner"] = $joi
}
if ($hasRanerInt8) { $vta["_comment_raner"] = "int8 reduce_range (torchcrf start/end npy)" }
$vta["defaultTable"] = "锅炉巡检"
$vta["denoiseEnabled"] = $false
$vta | ConvertTo-Json | Set-Content -Path (Join-Path $dst "vta.json") -Encoding UTF8
Write-Host "wrote assets/models/vta.json（首启随 assets 展开到 dataDir 生效）"

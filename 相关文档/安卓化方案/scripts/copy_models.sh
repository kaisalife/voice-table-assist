#!/usr/bin/env bash
# 拷贝模型与数据资源到 android/assets/models（方案 §4.1）；Linux/macOS 版
set -u
SRC="models"
DST="相关文档/安卓化方案/android/assets/models"
mkdir -p "$DST"

copy() {
  if [ -f "$SRC/$1" ]; then
    mkdir -p "$(dirname "$DST/$1")"
    cp -f "$SRC/$1" "$DST/$1"
    echo "copied $1"
  else
    echo "skip (missing): $1"
  fi
}

for d in "$SRC"/asr/sherpa-onnx-streaming-zipformer-zh-*/; do
  name=$(basename "$d")
  for f in encoder.int8.onnx encoder.onnx decoder.onnx joiner.int8.onnx joiner.onnx tokens.txt; do
    copy "asr/$name/$f"
  done
done
copy "asr/gtcrn_simple.onnx"
copy "asr/silero_vad.onnx"
# RaNER：优先 reduce_range 产物（规避 ORT x86 u8xs8 饱和缺陷）；否则 export-int8；否则 fp32
if [ -f "$SRC/raner/export-int8-reduced/model.onnx" ]; then
  RANER_SRC="$SRC/raner/export-int8-reduced"
  echo "RaNER: 使用 export-int8-reduced（98MB，已验证 x86/ARM 双平台可用）"
elif [ -f "$SRC/raner/export-int8/model.onnx" ]; then
  RANER_SRC="$SRC/raner/export-int8"
  echo "WARN RaNER: 回退 export-int8（未 reduce_range；ARM64 可用，x86 会饱和算错）"
else
  RANER_SRC=""
fi
if [ -n "$RANER_SRC" ]; then
  for f in model.onnx vocab.txt config.json crf_transitions.npy crf_start_transitions.npy crf_end_transitions.npy; do
    if [ -f "$RANER_SRC/$f" ]; then
      mkdir -p "$DST/raner"
      cp -f "$RANER_SRC/$f" "$DST/raner/$f"
    fi
  done
  echo "RaNER 就位: $RANER_SRC"
else
  echo "skip (no quantized artifact): falling back to fp32 raner/"
  copy "raner/model.onnx"
  copy "raner/vocab.txt"
  copy "raner/crf_transitions.json"
fi
copy "embedding/model_quantized.onnx"
copy "embedding/tokenizer.json"

for d in "$SRC"/embedding/tables/*/; do
  name=$(basename "$d")
  copy "embedding/tables/$name/cell_index.bin"
done
copy "embedding/tables/registry.json"

copy "sherpa-onnx/hr/hr_char_pinyin.txt"
copy "sherpa-onnx/hr/hr_common_rules.txt"
mkdir -p "$DST/sherpa-onnx/hr/tables/current"
[ -f "$DST/sherpa-onnx/hr/tables/current/hotwords.txt" ] || : > "$DST/sherpa-onnx/hr/tables/current/hotwords.txt"
echo "done -> $DST"

# 生成 vta.json：按实际拷入的文件自适应（ASR int8 优先，fp32 兜底）
ASR_REL=""; ENC=""; JOI=""
for d in "$DST"/asr/sherpa-onnx-streaming-zipformer-zh-*/; do
  name=$(basename "$d")
  if [ -f "$d/encoder.int8.onnx" ]; then ASR_REL="models/asr/$name"; ENC="encoder.int8.onnx"; JOI="joiner.int8.onnx"; break; fi
done
if [ -z "$ASR_REL" ]; then
  for d in "$DST"/asr/sherpa-onnx-streaming-zipformer-zh-*/; do
    name=$(basename "$d")
    if [ -f "$d/encoder.onnx" ]; then ASR_REL="models/asr/$name"; ENC="encoder.onnx"; JOI="joiner.onnx"; break; fi
  done
fi
{
  if [ -n "$ASR_REL" ]; then
    printf '"asrModelDir": "%s",\n"asrEncoder": "%s",\n"asrJoiner": "%s",\n' "$ASR_REL" "$ENC" "$JOI"
  fi
  if [ -f "$DST/raner/crf_transitions.npy" ]; then
    printf '"_comment_raner": "int8 (export-int8, torchcrf start/end npy)"\n'
  fi
} > "$DST/vta.json"
echo "wrote assets/models/vta.json"

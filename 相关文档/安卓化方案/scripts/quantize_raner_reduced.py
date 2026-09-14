# RaNER 量化：dynamic + reduce_range（规避 ORT x86 u8xs8 饱和缺陷）
#
# 背景：ORT 在 x86 上对 MatMulInteger(uint8, int8) 使用 VPMADDUBSW 指令，
# 当 A/B 两侧数值同时较大时中间累加溢出 int16 → 部分列结果错误。
# 这是 ORT 官方已知问题（issue #7524 / #16472 / #19109），AMD 部分 CPU 必现。
# 官方规避方案：reduce_range（权重限 7-bit，避免饱和）或 u8u8 格式。
#
# 用法（在 VoiceTableAssist 目录）：
#   python 相关文档\安卓化方案\scripts\quantize_raner_reduced.py [输入fp32.onnx] [输出目录]
# 默认：models/raner/model.onnx → models/raner/export-int8-reduced/
import os
import sys

from onnxruntime.quantization import QuantType, quantize_dynamic

SRC = sys.argv[1] if len(sys.argv) > 1 else 'models/raner/model.onnx'
OUT_DIR = sys.argv[2] if len(sys.argv) > 2 else 'models/raner/export-int8-reduced'
os.makedirs(OUT_DIR, exist_ok=True)
DST = os.path.join(OUT_DIR, 'model.onnx')

print(f'量化 {SRC} → {DST}（per_channel + reduce_range）...')
quantize_dynamic(SRC, DST, per_channel=True, reduce_range=True, weight_type=QuantType.QInt8)
print(f'完成：{os.path.getsize(DST)/1024/1024:.1f} MB')

# 拷贝配套文件（vocab / CRF 参数 / tokenizer）
import shutil
src_dir = os.path.dirname(SRC)
for f in ['vocab.txt', 'config.json', 'tokenizer.json', 'tokenizer_config.json',
          'crf_transitions.npy', 'crf_start_transitions.npy', 'crf_end_transitions.npy',
          'crf_transitions.json']:
    p = os.path.join(src_dir, f)
    if os.path.exists(p):
        shutil.copy2(p, os.path.join(OUT_DIR, f))
        print(f'  复制 {f}')
print('下一步验收：python 相关文档\\安卓化方案\\scripts\\raner_check.py --model %s' % DST)

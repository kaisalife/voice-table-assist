"""asr_ground_truth.py —— 判断"识别错误是模型层还是处理层"的基准工具。

用**官方 sherpa-onnx（pip 包）**加载与 App 完全相同的模型/解码参数，直接跑 WAV，
输出**未经本项目任何纠错**的原始文本；并可对比三种热词配置（无热词 / 本表热词 / 含单字数字）。

用途：出现"某个字被听没了/听错了"的疑问时，先跑这个脚本 ——
  - 若原始输出就已经错 → 模型层问题（本项目处理层不可能增删字符，见方案文档"数值识别边界"）；
  - 若原始输出正确、App 里错 → 才是本项目纠错/NER 之前处理的问题。

用法：
    python asr_ground_truth.py <wav目录>            # 目录内所有 16k/16bit/mono WAV
    需要 pip install sherpa-onnx numpy

生成测试语音：本仓库 selftest/tts.js 只是提示占位；Windows 上实际用 PowerShell
System.Speech（16000Hz/16bit/mono）合成，再转 float32 后可用 --selftest 跑设备端。
"""

import os
import sys
import wave

import numpy as np
import sherpa_onnx

MODEL = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "android", "assets", "models", "asr", "sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30",
)

# 汽机巡检表（6 行 4 列）——与 App 内 BuildHotWords 同格式：逐字空格分隔，/ 连接
ROWS = ["振动", "油温", "油压", "胀差", "真空", "轴向位移"]
ZH = ["一", "二", "三", "四"]
DIGITS = list("零一二三四五六七八九十") + ["点", "零"]


def descriptors():
    out = []
    for c in ZH:
        out += [c + "号", "第" + c + "个", "第" + c + "列", "测量值" + c, "序号" + c, c + "号列"]
    return out


def stream_hotwords(include_digits):
    phrases = ROWS + descriptors() + (DIGITS if include_digits else [])
    return "/".join(" ".join(p) for p in phrases)


def read_wav(path):
    with wave.open(path, "rb") as f:
        if f.getnchannels() != 1 or f.getsampwidth() != 2 or f.getframerate() != 16000:
            raise SystemExit(f"{path}: 需要 16kHz/16bit/mono WAV")
        data = f.readframes(f.getnframes())
    return np.frombuffer(data, dtype=np.int16).astype(np.float32) / 32768.0


def build_recognizer():
    if not os.path.isdir(MODEL):
        raise SystemExit(f"模型目录不存在: {MODEL}")
    return sherpa_onnx.OnlineRecognizer.from_transducer(
        tokens=os.path.join(MODEL, "tokens.txt"),
        encoder=os.path.join(MODEL, "encoder.int8.onnx"),
        decoder=os.path.join(MODEL, "decoder.onnx"),
        joiner=os.path.join(MODEL, "joiner.int8.onnx"),
        num_threads=4,
        sample_rate=16000,
        feature_dim=80,
        decoding_method="modified_beam_search",
        hotwords_score=2.0,
        modeling_unit="cjkchar",
        bpe_vocab="",
        enable_endpoint_detection=False,
    )


def run(rec, samples, hotwords):
    s = rec.create_stream(hotwords=hotwords) if hotwords else rec.create_stream()
    s.accept_waveform(16000, samples)
    s.input_finished()
    while rec.is_ready(s):
        rec.decode_stream(s)
    return rec.get_result(s)


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    directory = sys.argv[1]
    variants = [
        ("无热词", ""),
        ("本表热词(无数字)", stream_hotwords(False)),
        ("本表热词(含数字)", stream_hotwords(True)),
    ]
    rec = build_recognizer()
    for name in sorted(os.listdir(directory)):
        if not name.lower().endswith(".wav"):
            continue
        samples = read_wav(os.path.join(directory, name))
        print(f"{name}")
        for label, hw in variants:
            print(f"    {label:<18} -> {run(rec, samples, hw)!r}")


if __name__ == "__main__":
    main()

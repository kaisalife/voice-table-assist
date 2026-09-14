// jni/denoise/gtcrn_denoiser.h —— 翻译自 Asr/GtcrnDenoiser.cs
#pragma once

#include <memory>
#include <vector>

namespace vta {

// GTCRN (Grouped Temporal Convolutional Recurrent Network) 进程内 PCM 流式降噪器。
// 模型：sherpa-onnx 官方发布的 gtcrn_simple.onnx（48.2K 参数，33.0 MMACs/s，ICASSP 2024）。
//
// 移植自 sherpa-onnx online-speech-denoiser-gtcrn-impl.h + online-speech-denoiser-stft-impl.h：
//   - STFT：窗 √hann（hann_sqrt），n_fft=512，hop=256，window=512（16kHz）；
//   - 每 hop 进分析窗 → 前向 DFT → ONNX → 逆向 DFT → √hann 加窗 → overlap-add → 输出 hop 段；
//   - 状态缓存 conv_cache/tra_cache/inter_cache 跨帧携带；
//   - 帧对齐/补零/收尾 flush 逻辑与 sherpa 参考一致（16kHz 输入 → 无需重采样）。
// 设计目标：与 sherpa-onnx 离线降噪输出数值一致（误差 < 1e-4），可被 selftest 对照验证。
class GtcrnDenoiser {
public:
    // numThreads 传 ONNX Runtime intra-op 线程数；sampleRate 必须为 16000。
    GtcrnDenoiser(const std::string& modelPath, int numThreads, int sampleRate);
    ~GtcrnDenoiser();
    GtcrnDenoiser(const GtcrnDenoiser&) = delete;
    GtcrnDenoiser& operator=(const GtcrnDenoiser&) = delete;

    // 复位所有状态缓存（GRU/DFT/overlap-add）。在新一轮对话开始时调用。
    void Reset();

    // 对一段 16kHz float32 PCM 样本（[-1,1]）做流式降噪，返回降噪后的样本段。
    // flush=true 时做收尾（补零 + 尾帧 + 复位），用于音频段结束。
    std::vector<float> Denoise(const float* samples, size_t n, bool flush = false);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    void ProcessHop(std::vector<float>* output);
    void ProcessHopBuffer(const float* hop, std::vector<float>* output);
    void ProcessFrame();
    void ForwardDft(const float* input, float* output);
    void InverseDft(const float* input, float* output);
    void ResetCaches();
};

}  // namespace vta

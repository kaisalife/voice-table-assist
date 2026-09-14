// jni/denoise/gtcrn_denoiser.cc —— 翻译自 Asr/GtcrnDenoiser.cs（逐行对齐）
#include "gtcrn_denoiser.h"

#include <cmath>
#include <cstring>
#include <stdexcept>
#include <onnxruntime_cxx_api.h>

#include "../common/log.h"

namespace vta {

namespace {
constexpr int kNfft = 512;
constexpr int kHopLength = 256;
constexpr int kWindowLength = 512;
constexpr int kNumBins = kNfft / 2 + 1;   // 257
constexpr int kSpecSize = kNumBins * 2;   // 514
}  // namespace

struct GtcrnDenoiser::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "vta-gtcrn"};
    std::unique_ptr<Ort::Session> session;

    // ---- STFT 状态（对齐 OnlineSpeechDenoiserStftImpl） ----
    std::vector<float> analysisBuffer = std::vector<float>(kWindowLength, 0.f);
    std::vector<float> overlapAddBuffer = std::vector<float>(kWindowLength, 0.f);
    std::vector<float> fftInput = std::vector<float>(kWindowLength, 0.f);
    std::vector<float> fftOutput = std::vector<float>(kSpecSize, 0.f);
    std::vector<float> enhancedFftOutput = std::vector<float>(kSpecSize, 0.f);
    std::vector<float> ifftOutput = std::vector<float>(kWindowLength, 0.f);
    std::vector<float> window;             // MakeHannSqrtWindow
    std::vector<float> pendingInput;
    bool started = false;
    long long totalInputSamples = 0;
    long long totalOutputSamples = 0;
    std::vector<float> zeroHop = std::vector<float>(kHopLength, 0.f);

    // ---- DFT 预计算表（对齐 StreamingDft） ----
    std::vector<double> cosF, sinF, cosI, sinI;

    // ---- ONNX 状态缓存 ----
    std::vector<float> convCache;   // [2,1,16,16,33]
    std::vector<float> traCache;    // [2,3,1,1,16]
    std::vector<float> interCache;  // [2,1,33,16]
    static constexpr int64_t kConvCacheElems = 2 * 1 * 16 * 16 * 33;
    static constexpr int64_t kTraCacheElems = 2 * 3 * 1 * 1 * 16;
    static constexpr int64_t kInterCacheElems = 2 * 1 * 33 * 16;
};

GtcrnDenoiser::GtcrnDenoiser(const std::string& modelPath, int numThreads, int sampleRate)
    : impl_(new Impl()) {
    if (sampleRate != 16000)
        throw std::runtime_error("GTCRN 仅支持 16kHz，实际 " + std::to_string(sampleRate));

    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(numThreads > 0 ? numThreads : 1);
    opts.SetLogSeverityLevel(2);  // WARNING
    impl_->session = std::make_unique<Ort::Session>(impl_->env, modelPath.c_str(), opts);

    // 预计算 DFT 表（StreamingDft 构造）
    const double pi = M_PI;
    impl_->cosF.resize(static_cast<size_t>(kNumBins) * kNfft);
    impl_->sinF.resize(static_cast<size_t>(kNumBins) * kNfft);
    impl_->cosI.resize(static_cast<size_t>(kNfft) * kNumBins);
    impl_->sinI.resize(static_cast<size_t>(kNfft) * kNumBins);
    for (int k = 0; k < kNumBins; ++k) {
        for (int n = 0; n < kNfft; ++n) {
            double angle = 2.0 * pi * k * n / kNfft;
            double c = std::cos(angle);
            double s = std::sin(angle);
            impl_->cosF[static_cast<size_t>(k) * kNfft + n] = c;
            impl_->sinF[static_cast<size_t>(k) * kNfft + n] = s;
            impl_->cosI[static_cast<size_t>(n) * kNumBins + k] = c;
            impl_->sinI[static_cast<size_t>(n) * kNumBins + k] = s;
        }
    }

    // √hann 窗
    impl_->window.resize(kWindowLength);
    for (int i = 0; i < kWindowLength; ++i) {
        double h = 0.5 * (1.0 - std::cos(2.0 * M_PI * i / (kWindowLength - 1)));
        impl_->window[i] = static_cast<float>(std::sqrt(h));
    }

    ResetCaches();
}

GtcrnDenoiser::~GtcrnDenoiser() = default;

void GtcrnDenoiser::ResetCaches() {
    // 与 sherpa-onnx 导出 ONNX 的形状严格一致
    impl_->convCache.assign(Impl::kConvCacheElems, 0.f);
    impl_->traCache.assign(Impl::kTraCacheElems, 0.f);
    impl_->interCache.assign(Impl::kInterCacheElems, 0.f);
}

void GtcrnDenoiser::Reset() {
    std::fill(impl_->analysisBuffer.begin(), impl_->analysisBuffer.end(), 0.f);
    std::fill(impl_->overlapAddBuffer.begin(), impl_->overlapAddBuffer.end(), 0.f);
    impl_->pendingInput.clear();
    impl_->started = false;
    impl_->totalInputSamples = 0;
    impl_->totalOutputSamples = 0;
    ResetCaches();
}

std::vector<float> GtcrnDenoiser::Denoise(const float* samples, size_t n, bool flush) {
    impl_->totalInputSamples += static_cast<long long>(n);
    impl_->pendingInput.insert(impl_->pendingInput.end(), samples, samples + n);

    std::vector<float> output;
    output.reserve(n);

    // ProcessPending：每满一个 hop 处理一帧
    while (impl_->pendingInput.size() >= static_cast<size_t>(kHopLength)) {
        ProcessHop(&output);
    }

    if (flush) {
        // Flush：补零到 hop 处理剩余，再处理一个 zero-hop 收尾
        if (!impl_->pendingInput.empty()) {
            std::vector<float> padded(kHopLength, 0.f);
            std::copy(impl_->pendingInput.begin(), impl_->pendingInput.end(), padded.begin());
            ProcessHopBuffer(padded.data(), &output);
            impl_->pendingInput.clear();
        }

        if (impl_->started) ProcessHopBuffer(impl_->zeroHop.data(), &output);

        // 只输出实际输入量（对齐 sherpa Flush 的 remaining 裁剪）
        long long remaining = impl_->totalInputSamples - impl_->totalOutputSamples;
        if (remaining < 0) remaining = 0;
        if (static_cast<long long>(output.size()) > remaining)
            output.resize(static_cast<size_t>(remaining));
        impl_->totalOutputSamples += static_cast<long long>(output.size());

        Reset();
    }

    return output;
}

void GtcrnDenoiser::ProcessHop(std::vector<float>* output) {
    // 从 pendingInput 消费一个 hop
    float hop[kHopLength];
    for (int i = 0; i < kHopLength; ++i) hop[i] = impl_->pendingInput[i];
    impl_->pendingInput.erase(impl_->pendingInput.begin(),
                              impl_->pendingInput.begin() + kHopLength);
    ProcessHopBuffer(hop, output);
}

void GtcrnDenoiser::ProcessHopBuffer(const float* hop, std::vector<float>* output) {
    // 1) 滑动分析窗：左移 hop，右侧填入新 hop
    std::copy(impl_->analysisBuffer.begin() + kHopLength, impl_->analysisBuffer.end(),
              impl_->analysisBuffer.begin());
    for (int i = 0; i < kHopLength; ++i)
        impl_->analysisBuffer[kWindowLength - kHopLength + i] = hop[i];

    // 2) 窗乘
    for (int i = 0; i < kWindowLength; ++i)
        impl_->fftInput[i] = impl_->analysisBuffer[i] * impl_->window[i];

    // 3) 前向 DFT
    ForwardDft(impl_->fftInput.data(), impl_->fftOutput.data());

    // 4) ONNX 推理（mix + 3 caches）→ enh + 3 cache_out
    ProcessFrame();

    // 5) 逆向 DFT
    InverseDft(impl_->enhancedFftOutput.data(), impl_->ifftOutput.data());

    // 6) overlap-add：左移 hop，原位清空尾部，叠加 ifft*window
    std::copy(impl_->overlapAddBuffer.begin() + kHopLength, impl_->overlapAddBuffer.end(),
              impl_->overlapAddBuffer.begin());
    std::fill(impl_->overlapAddBuffer.end() - kHopLength, impl_->overlapAddBuffer.end(), 0.f);
    for (int i = 0; i < kWindowLength; ++i)
        impl_->overlapAddBuffer[i] += impl_->ifftOutput[i] * impl_->window[i];

    // 7) 第一次调用不输出（started），之后输出前 hop 段
    if (!impl_->started) {
        impl_->started = true;
        return;
    }

    output->insert(output->end(), impl_->overlapAddBuffer.begin(),
                   impl_->overlapAddBuffer.begin() + kHopLength);
}

void GtcrnDenoiser::ProcessFrame() {
    // mix = [1, 257, 1, 2]，从 fftOutput 拷贝
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    std::vector<int64_t> mixShape{1, kNumBins, 1, 2};
    Ort::Value mix = Ort::Value::CreateTensor<float>(mem, impl_->fftOutput.data(), kSpecSize,
                                                     mixShape.data(), 4);

    std::vector<int64_t> convShape{2, 1, 16, 16, 33};
    std::vector<int64_t> traShape{2, 3, 1, 1, 16};
    std::vector<int64_t> interShape{2, 1, 33, 16};
    Ort::Value conv = Ort::Value::CreateTensor<float>(mem, impl_->convCache.data(),
                                                      Impl::kConvCacheElems, convShape.data(), 5);
    Ort::Value tra = Ort::Value::CreateTensor<float>(mem, impl_->traCache.data(),
                                                     Impl::kTraCacheElems, traShape.data(), 5);
    Ort::Value inter = Ort::Value::CreateTensor<float>(mem, impl_->interCache.data(),
                                                       Impl::kInterCacheElems, interShape.data(), 4);

    const char* inputNames[] = {"mix", "conv_cache", "tra_cache", "inter_cache"};
    const char* outputNames[] = {"enh", "conv_cache_out", "tra_cache_out", "inter_cache_out"};
    // Run 的 input_values 是连续数组首指针 → 用 vector<Ort::Value>
    std::vector<Ort::Value> inputs;
    inputs.push_back(std::move(mix));
    inputs.push_back(std::move(conv));
    inputs.push_back(std::move(tra));
    inputs.push_back(std::move(inter));

    auto results = impl_->session->Run(Ort::RunOptions{nullptr}, inputNames, inputs.data(), 4,
                                       outputNames, 4);

    // enh → enhancedFftOutput
    {
        auto info = results[0].GetTensorTypeAndShapeInfo();
        size_t count = info.GetElementCount();
        const float* enh = results[0].GetTensorData<float>();
        std::copy(enh, enh + count, impl_->enhancedFftOutput.begin());
    }
    // caches 原位替换
    std::copy(results[1].GetTensorData<float>(),
              results[1].GetTensorData<float>() + Impl::kConvCacheElems, impl_->convCache.begin());
    std::copy(results[2].GetTensorData<float>(),
              results[2].GetTensorData<float>() + Impl::kTraCacheElems, impl_->traCache.begin());
    std::copy(results[3].GetTensorData<float>(),
              results[3].GetTensorData<float>() + Impl::kInterCacheElems, impl_->interCache.begin());
}

void GtcrnDenoiser::ForwardDft(const float* input, float* output) {
    for (int k = 0; k < kNumBins; ++k) {
        double real = 0, imag = 0;
        size_t off = static_cast<size_t>(k) * kNfft;
        for (int n = 0; n < kNfft; ++n) {
            double v = input[n];
            real += v * impl_->cosF[off + n];
            imag -= v * impl_->sinF[off + n];
        }
        output[2 * k] = static_cast<float>(real);
        output[2 * k + 1] = static_cast<float>(imag);
    }
}

void GtcrnDenoiser::InverseDft(const float* input, float* output) {
    for (int n = 0; n < kNfft; ++n) {
        double sum = input[0];
        if (kNfft % 2 == 0)
            sum += input[2 * (kNumBins - 1)] * ((n & 1) != 0 ? -1.0 : 1.0);

        size_t off = static_cast<size_t>(n) * kNumBins;
        for (int k = 1; k < kNumBins - 1; ++k) {
            double real = input[2 * k];
            double imag = input[2 * k + 1];
            sum += 2.0 * (real * impl_->cosI[off + k] - imag * impl_->sinI[off + k]);
        }
        output[n] = static_cast<float>(sum / kNfft);
    }
}

}  // namespace vta

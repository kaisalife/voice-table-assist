// jni/asr/recognizer.cc
#include "recognizer.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <stdexcept>

#include "sherpa-onnx/csrc/online-recognizer.h"

#include "../common/log.h"

namespace vta {

struct AsrRecognizer::Impl {
    std::unique_ptr<sherpa_onnx::OnlineRecognizer> recognizer;
};

// StreamHolder 必须在 Stream 成员函数定义之前完整可见
struct StreamHolder {
    explicit StreamHolder(std::unique_ptr<sherpa_onnx::OnlineStream> p) : s(std::move(p)) {}
    std::unique_ptr<sherpa_onnx::OnlineStream> s;
};

AsrRecognizer::AsrRecognizer(const Config& config) : impl_(new Impl()), config_(config) {
    sherpa_onnx::OnlineRecognizerConfig c;

    // transducer 三件套（encoder/decoder/joiner 在 OnlineTransducerModelConfig 里）
    c.model_config.transducer.encoder = config.modelDir + "/" + config.encoder;
    c.model_config.transducer.decoder = config.modelDir + "/" + config.decoder;
    c.model_config.transducer.joiner = config.modelDir + "/" + config.joiner;
    c.model_config.tokens = config.modelDir + "/" + config.tokens;
    c.model_config.num_threads = config.numThreads;
    c.model_config.provider_config.provider = "cpu";
    c.model_config.debug = false;
    // 建模单元必须与模型 tokens.txt 匹配（cjkchar/bbpe）；bbpe 需 bpeVocab，否则热词编码会崩
    c.model_config.modeling_unit = config.modelingUnit;
    c.model_config.bpe_vocab = config.bpeVocab;

    c.decoding_method = config.decodingMethod;

    // 端点：与现网 SherpaServerOptions 一致——三条规则同阈值，每句停顿即切句输出 final
    c.enable_endpoint = config.enableEndpoint;
    c.endpoint_config.rule1.min_trailing_silence = static_cast<float>(config.rule1TrailingSilence);
    c.endpoint_config.rule2.min_trailing_silence = static_cast<float>(config.rule2TrailingSilence);
    c.endpoint_config.rule3.min_trailing_silence = static_cast<float>(config.rule3TrailingSilence);
    c.endpoint_config.rule1.must_contain_nonsilence = true;
    c.endpoint_config.rule2.must_contain_nonsilence = true;
    c.endpoint_config.rule3.must_contain_nonsilence = false;

    // 热词不在此处加载：改为按流（per-stream）在 CreateStream(hotwords) 传入，
    // 这样识别器常驻常驻、切表/导入都不需要重建。
    c.hotwords_file = "";
    c.hotwords_score = static_cast<float>(config.hotwordsScore);

    // sherpa 内置 HR（可选；默认空，同音纠正走本进程 HomophoneReplacer）
    c.hr.lexicon = config.hrLexicon;
    c.hr.rule_fsts = config.hrRuleFsts;

    impl_->recognizer = std::make_unique<sherpa_onnx::OnlineRecognizer>(c);
    ALOGI("[ASR] OnlineRecognizer ready: dir=%s threads=%d modeling_unit=%s (热词按流传入)", config.modelDir.c_str(),
          config.numThreads, config.modelingUnit.c_str());
}

AsrRecognizer::~AsrRecognizer() = default;

AsrRecognizer::Stream::Stream(const AsrRecognizer* owner, void* impl)
    : owner_(owner), impl_(new StreamHolder(std::unique_ptr<sherpa_onnx::OnlineStream>(
          static_cast<sherpa_onnx::OnlineStream*>(impl)))) {}

AsrRecognizer::Stream::~Stream() { delete static_cast<StreamHolder*>(impl_); }

void AsrRecognizer::Stream::AcceptWaveform(const float* samples, int n) {
    auto* h = static_cast<StreamHolder*>(impl_);
    h->s->AcceptWaveform(16000, samples, n);
}

void AsrRecognizer::Stream::InputFinished() {
    auto* h = static_cast<StreamHolder*>(impl_);
    h->s->InputFinished();
}

std::unique_ptr<AsrRecognizer::Stream> AsrRecognizer::CreateStream() const {
    auto s = impl_->recognizer->CreateStream();
    return std::unique_ptr<Stream>(new Stream(this, s.release()));
}

std::unique_ptr<AsrRecognizer::Stream> AsrRecognizer::CreateStream(
    const std::string& hotwords) const {
    auto s = hotwords.empty() ? impl_->recognizer->CreateStream()
                              : impl_->recognizer->CreateStream(hotwords);
    return std::unique_ptr<Stream>(new Stream(this, s.release()));
}

std::string AsrRecognizer::PumpDecode(Stream* stream) const {
    auto* h = static_cast<StreamHolder*>(stream->impl_);
    auto* s = h->s.get();
    // IsReady 在 recognizer 上（v1.12.x API）
    while (impl_->recognizer->IsReady(s)) {
        impl_->recognizer->DecodeStream(s);
    }
    return impl_->recognizer->GetResult(s).text;
}

bool AsrRecognizer::IsEndpoint(Stream* stream) const {
    if (!config_.enableEndpoint) return false;
    auto* h = static_cast<StreamHolder*>(stream->impl_);
    return impl_->recognizer->IsEndpoint(h->s.get());
}

std::string AsrRecognizer::TakeEndpointResult(Stream* stream) const {
    auto* h = static_cast<StreamHolder*>(stream->impl_);
    auto* s = h->s.get();
    std::string text = impl_->recognizer->GetResult(s).text;
    impl_->recognizer->Reset(s);
    return text;
}

void AsrRecognizer::ResetStream(Stream* stream) const {
    auto* h = static_cast<StreamHolder*>(stream->impl_);
    impl_->recognizer->Reset(h->s.get());
}

// ---------------- PcmPipe ----------------

PcmPipe::PcmPipe(size_t maxSamples) : maxSamples_(maxSamples) {}

void PcmPipe::Push(const float* samples, size_t n) {
    if (n == 0) return;  // 喂空 PCM 块：不崩，直接忽略（方案 §8.4）
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (closed_) return;
        buf_.insert(buf_.end(), samples, samples + n);
        if (buf_.size() > maxSamples_) {
            size_t drop = buf_.size() - maxSamples_;
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(drop));
        }
    }
    cv_.notify_all();
}

size_t PcmPipe::Pop(float* dst, size_t maxN, int timeoutMs) {
    std::unique_lock<std::mutex> lk(mu_);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (buf_.empty() && !closed_) {
        if (cv_.wait_until(lk, deadline) == std::cv_status::timeout) break;
    }
    size_t take = std::min(maxN, buf_.size());
    if (take > 0) {
        std::copy(buf_.begin(), buf_.begin() + static_cast<long>(take), dst);
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(take));
    }
    return take;
}

void PcmPipe::Close() {
    {
        std::lock_guard<std::mutex> lk(mu_);
        closed_ = true;
    }
    cv_.notify_all();
}

bool PcmPipe::Closed() const {
    std::lock_guard<std::mutex> lk(mu_);
    return closed_;
}

size_t PcmPipe::Buffered() const {
    std::lock_guard<std::mutex> lk(mu_);
    return buf_.size();
}

}  // namespace vta

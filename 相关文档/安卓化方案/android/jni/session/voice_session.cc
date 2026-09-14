// jni/session/voice_session.cc —— 等价 SherpaAsrBridge.RunAsync 会话侧 + 识别泵
#include "voice_session.h"

#include <chrono>

#include "../common/log.h"
#include "../common/strings.h"
#include "../denoise/gtcrn_denoiser.h"
#include "../homophone/domain_correction.h"
#include "../service/engine_host.h"
#include "../tables/table_vector_manager.h"
#include "../tables/voice_resource.h"
#include "../text/pinyin_table.h"
#include "../text/sound_aligner.h"

namespace vta {

namespace {
constexpr size_t kPopChunk = 1600;  // 100ms @16kHz
}  // namespace

std::unique_ptr<VoiceSession> VoiceSession::CreateAndStart(int sessionId,
                                                           const VoiceSessionConfig& cfg,
                                                           EngineHost* host,
                                                           std::shared_ptr<const VtxIndex> index,
                                                           std::string* err) {
    auto s = std::unique_ptr<VoiceSession>(new VoiceSession());
    s->config_ = cfg;
    s->config_.sessionId = sessionId;
    s->host_ = host;
    s->index_ = std::move(index);

    auto asr = host->Asr();
    if (asr == nullptr) {
        if (err) *err = "ASR 识别器未加载";
        return nullptr;
    }
    s->asr_ = asr;  // 共享持有：识别器被重建时本会话的流仍然有效

    // 本表热词（按流传词，识别器不重建）：内容与现网 C# BuildHotWords 完全一致
    std::string hotwords;
    if (s->index_ != nullptr) {
        hotwords = TableVoiceResourceGenerator::BuildHotWordsStream(
            s->index_->rows, s->index_->colsCount, cfg.hotwordDigits, cfg.hotwordDigitTen);
    }
    s->stream_ = asr->CreateStream(hotwords);
    if (!s->stream_) {
        if (err) *err = "创建识别流失败";
        return nullptr;
    }

    std::string derr;
    auto* d = host->NewDenoiser(&derr);
    if (d != nullptr) s->denoiser_.reset(d);
    // 降噪器进程级单例语义（C#）：每会话开始必须复位 —— 此处每会话新实例天然等价

    // 表内读音对齐器：词表 = 本表行标签 + 列说法（自动生成，零人工维护）
    if (!cfg.pinyinPath.empty() && s->index_ != nullptr) {
        auto py = PinyinTable::Get(cfg.pinyinPath);
        if (py != nullptr) {
            s->aligner_ =
                std::make_shared<SoundAligner>(py, s->index_->rows, s->index_->colsCount);
        } else {
            ALOGW("[ALIGN] 拼音字典不可用，跳过读音对齐: %s", cfg.pinyinPath.c_str());
        }
    }

    s->interaction_ = std::make_unique<VoiceInteractionSession>(
        cfg.silenceMs, cfg.maxChars, cfg.minSim, host->Raner(), host->Embed(), s->index_,
        s->aligner_);
    s->phase_ = "listening";
    s->worker_ = std::thread([p = s.get()] { p->Run(); });
    ALOGI("[SESSION] #%d opened table=%s key=%s captureMode=%d silenceMs=%d", sessionId,
          cfg.tableName.c_str(), cfg.tableKey.c_str(), cfg.captureMode, cfg.silenceMs);
    return s;
}

VoiceSession::~VoiceSession() {
    Close();
    if (worker_.joinable()) worker_.join();
}

void VoiceSession::PushPcm(const float* samples, size_t n) {
    pipe_.Push(samples, n);
    if (!phase_.empty() && phase_ == "endpointing") phase_ = "listening";
}

void VoiceSession::OnCapturedAudio(const float* samples, int n) { PushPcm(samples, n); }

VoiceSession::State VoiceSession::GetState() const {
    std::lock_guard<std::mutex> lk(stateMu_);
    State st;
    st.phase = phase_;
    st.partial = partial_;
    st.lastPartialMs = lastPartialMs_;
    return st;
}

std::string VoiceSession::LastFinalText() const {
    std::lock_guard<std::mutex> lk(stateMu_);
    return lastFinalText_;
}

void VoiceSession::Close() {
    if (closeRequested_.exchange(true)) return;
    pipe_.Close();
    // 等 worker 收尾完成（flush + 提交），与现网 stop 语义一致
    while (!finished_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (worker_.joinable()) worker_.join();
}

void VoiceSession::Run() {
    AsrRecognizer* asr = asr_.get();  // 本会话共享持有的识别器（重建也不受影响）
    std::vector<float> buf;

    // 输入段性能指标：每轮解码耗时（平均/最大）——这才是"解码是否卡顿"的判据。
    // 注意：不要用"partial 间隔"当指标——说话本来每 200~400ms 才出一个字，
    // 文本变化的间隔天然是这个量级，会误判成卡顿。
    int decodeRounds = 0;
    double decodeTotalMs = 0, decodeMaxMs = 0;

    // 纠错函数：数字/列号语境归一 → **表内读音吸附**
    // 吸附同时作用于 partial 与 final：让界面"实时识别"看到的就是最终会用来解析的文本
    // （否则用户看到"气压"、格子里却填"汽压"，会以为纠错没生效）。
    auto correct = [&](const std::string& raw) {
        std::string text = DomainCorrect(raw);
        text = DomainCorrect(text);
        if (aligner_ && aligner_->Ready()) text = aligner_->AlignSentence(text);
        return text;
    };

    std::string lastPartialRaw;  // 最近一条 partial 原文（stop 时 fold 进累计）

    // 诊断：纠错改变了文本时，同时打出 ASR 原文。
    // 用途：区分"模型听错"与"我们改错"——此前只打纠错后文本，排查"点丢失"这类问题时
    // 无法判断错在 ASR 还是纠错层，只能靠外部工具离线复现。行为中性（仅日志）。
    auto logRawIfChanged = [](const char* tag, const std::string& raw, const std::string& text) {
        if (raw != text) ALOGI("[ASR] raw(%s): %s", tag, raw.c_str());
    };

    auto emitPartial = [&](const std::string& raw) {
        std::string text = correct(raw);
        logRawIfChanged("partial", raw, text);
        ALOGD("[ASR] partial: %s", text.c_str());
        std::lock_guard<std::mutex> lk(stateMu_);
        partial_ = text;
        lastPartialMs_ = NowMs();
        phase_ = "listening";
    };

    auto emitFinal = [&](const std::string& raw) {
        std::string text = correct(raw);
        logRawIfChanged("final", raw, text);
        ALOGI("[ASR] final: %s", text.c_str());
        {
            std::lock_guard<std::mutex> lk(stateMu_);
            phase_ = "endpointing";
            partial_.clear();
            lastFinalText_ = text;  // 供自检/日志展示
        }
        interaction_->OnFinal(text);
        lastPartialRaw.clear();
    };

    while (!closeRequested_.load()) {
        auto loop0 = std::chrono::steady_clock::now();
        buf.resize(kPopChunk);
        size_t n = pipe_.Pop(buf.data(), kPopChunk, 100);
        if (n > 0) {
            if (denoiser_) {
                auto denoised = denoiser_->Denoise(buf.data(), n);
                if (!denoised.empty()) stream_->AcceptWaveform(denoised.data(), (int)denoised.size());
            } else {
                stream_->AcceptWaveform(buf.data(), (int)n);
            }
        }

        // 解码泵 + partial
        std::string text = asr->PumpDecode(stream_.get());
        if (!text.empty() && text != lastPartialRaw) {
            lastPartialRaw = text;
            emitPartial(text);
        }
        auto loop1 = std::chrono::steady_clock::now();

        // 端点切句：final → 交互编排（静默计时器后续提交）
        if (asr->IsEndpoint(stream_.get())) {
            std::string finalText = asr->TakeEndpointResult(stream_.get());
            if (!finalText.empty()) emitFinal(finalText);
        }
        auto loop2 = std::chrono::steady_clock::now();
        auto msf = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        double totalMs = msf(loop0, loop2);
        ++decodeRounds;
        decodeTotalMs += totalMs;
        if (totalMs > decodeMaxMs) decodeMaxMs = totalMs;
        // 单轮明显超时（>200ms 而分块仅 100ms）→ 说明解码被拖住，拆分定位来源
        if (totalMs > 200) {
            ALOGI("[PERF] 单轮 %.0fms = 解码 %.0fms + 端点复位 %.0fms", totalMs, msf(loop0, loop1),
                  msf(loop1, loop2));
        }
    }

    // ---- 收尾（等价客户端 {"type":"stop"}）----
    // 排空管道：主线程可能在 worker 消费前就 Close（push 完立即 close），
    // 剩余音频必须全部过完识别再收尾，否则丢句。
    size_t drained = 0;
    while (true) {
        auto loop0 = std::chrono::steady_clock::now();
        buf.resize(kPopChunk);
        size_t n = pipe_.Pop(buf.data(), kPopChunk, 0);
        if (n == 0) break;
        drained += n;
        if (denoiser_) {
            auto denoised = denoiser_->Denoise(buf.data(), n);
            if (!denoised.empty()) stream_->AcceptWaveform(denoised.data(), (int)denoised.size());
        } else {
            stream_->AcceptWaveform(buf.data(), (int)n);
        }
        std::string text = asr->PumpDecode(stream_.get());
        if (!text.empty() && text != lastPartialRaw) {
            lastPartialRaw = text;
            emitPartial(text);
        }
        if (asr->IsEndpoint(stream_.get())) {
            std::string finalText = asr->TakeEndpointResult(stream_.get());
            if (!finalText.empty()) emitFinal(finalText);
        }
        // 排空循环也计入解码耗时统计（文件喂流/收尾时解码主要发生在这里）
        {
            double totalMs =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - loop0)
                    .count();
            ++decodeRounds;
            decodeTotalMs += totalMs;
            if (totalMs > decodeMaxMs) decodeMaxMs = totalMs;
            if (totalMs > 200) ALOGI("[PERF] 排空单轮 %.0fms", totalMs);
        }
    }
    if (drained > 0) ALOGI("[ASR] 收尾排空 %zu 样本", drained);
    // 降噪器 flush：把 < hop 的尾帧补零收尾并复位状态（输出丢弃，仅为清态）。
    if (denoiser_) {
        try { denoiser_->Denoise(nullptr, 0, /*flush=*/true); } catch (...) {}
    }
    // 先让识别输出最终 final（一般 <1s），再立即提交——否则此刻累计为空。
    stream_->InputFinished();
    AsrRecognizer* asrP = asr_.get();
    std::string finalText;
    if (asrP != nullptr) {
        // PumpDecode 内部排空所有就绪帧；InputFinished 后取到的即最终文本
        std::string t = asrP->PumpDecode(stream_.get());
        if (!t.empty()) finalText = t;
    }
    if (!finalText.empty()) emitFinal(finalText);  // 走统一出口：记录 final 文本供自检/日志
    // 把最近一条还没进累计的 partial 文本补 fold（partial 只下发 UI，不进累计）。
    if (!lastPartialRaw.empty()) interaction_->FoldPartial(correct(lastPartialRaw));
    interaction_->Flush();

    {
        std::lock_guard<std::mutex> lk(stateMu_);
        phase_ = "closed";
        partial_.clear();
    }
    finished_ = true;
    ALOGI("[PERF] 语音输入段: %d 轮解码，平均 %.0fms/轮，最大 %.0fms/轮（分块 100ms；最大明显偏大=解码卡顿）",
          decodeRounds, decodeRounds ? decodeTotalMs / decodeRounds : 0.0, decodeMaxMs);
    ALOGI("[SESSION] #%d closed", config_.sessionId);
}

}  // namespace vta

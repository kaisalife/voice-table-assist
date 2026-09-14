// jni/session/voice_session.h —— 一路语音会话的进程内编排（等价 SherpaAsrBridge.RunAsync 的会话侧）
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "../asr/recognizer.h"
#include "../session/interaction_session.h"

namespace vta {

class AsrRecognizer;
class EngineHost;
class GtcrnDenoiser;
class SoundAligner;
class VtxIndex;

// captureMode: 0 = Service 自主采音（AAudio 回调喂 pipe）；1 = 企业 App pushPcm 喂流
struct VoiceSessionConfig {
    int sessionId = 0;
    std::string tableName;
    std::string tableKey;
    int silenceMs = 300;
    int maxChars = 500;
    float minSim = 0.55f;
    int captureMode = 0;
    std::string pinyinPath;  // 拼音字典（表内读音对齐用；空 = 不做对齐）
    bool hotwordDigits = false;  // 是否把单字数字写入热词（默认 false）
    bool hotwordDigitTen = false;  // 是否额外写"十"（BBPE 模型默认不写；换模型 A/B 用）
};

// 生命周期：openSession → CreateAndStart → pushPcm/采音 → pollCells → closeSession
class VoiceSession {
public:
    // host 提供识别器/降噪器/语义引擎；index 为该表索引快照（共享所有权）
    static std::unique_ptr<VoiceSession> CreateAndStart(int sessionId, const VoiceSessionConfig& cfg,
                                                        EngineHost* host,
                                                        std::shared_ptr<const VtxIndex> index,
                                                        std::string* err);
    ~VoiceSession();

    // captureMode=1：企业 App 喂流（16kHz float32）
    void PushPcm(const float* samples, size_t n);
    // captureMode=0：AAudio 回调转发入口（由服务实现层路由）
    void OnCapturedAudio(const float* samples, int n);
    int CaptureMode() const { return config_.captureMode; }

    // AIDL getState
    struct State {
        std::string phase;      // "idle"/"listening"/"endpointing"/"closed"
        std::string partial;
        int64_t lastPartialMs = 0;
    };
    State GetState() const;

    std::vector<CellHitInternal> PollCells(int maxN, int timeoutMs) {
        return interaction_->PollCells(maxN, timeoutMs);
    }

    // 收尾：flush 降噪/识别，fold partial，提交剩余 cells（等价客户端 {"type":"stop"}）
    void Close();

    int Id() const { return config_.sessionId; }
    const std::string& TableName() const { return config_.tableName; }
    // 最近一段 final 文本（纠错后）；自检/日志用
    std::string LastFinalText() const;

private:
    void Run();  // 识别工作线程主循环

    VoiceSessionConfig config_;
    EngineHost* host_;
    std::shared_ptr<const VtxIndex> index_;      // 共享所有权：多表并发不悬空
    std::shared_ptr<AsrRecognizer> asr_;        // 共享所有权：识别器重建时不悬空

    std::unique_ptr<AsrRecognizer::Stream> stream_;
    std::unique_ptr<GtcrnDenoiser> denoiser_;
    std::unique_ptr<VoiceInteractionSession> interaction_;
    std::shared_ptr<const SoundAligner> aligner_;  // 表内读音对齐器（随会话表构建）
    PcmPipe pipe_;

    std::thread worker_;
    std::atomic<bool> closeRequested_{false};
    std::atomic<bool> finished_{false};

    mutable std::mutex stateMu_;
    std::string phase_ = "idle";
    std::string partial_;
    int64_t lastPartialMs_ = 0;
    std::string lastFinalText_;
    std::string lastAsrText_;  // 最近一条未纠正的 ASR 文本（partial fold 用原文本？——用纠正后文本，与现网一致）
};

}  // namespace vta

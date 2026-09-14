// jni/service/engine_host.h —— 翻译自 Services/EngineHost.cs
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "../asr/recognizer.h"
#include "vta_config.h"

namespace vta {

class RaNerEngine;
class EmbeddingEngine;
class GtcrnDenoiser;

// 引擎宿主：RaNER + gte-base-zh 语义引擎按需加载、空闲卸载（默认 180s，idleUnloadSeconds 可配）。
// ASR 识别器常驻（懒加载一次后不卸载）。空闲判定：距最后一次使用超过阈值，且无活跃语音会话。
class EngineHost {
public:
    explicit EngineHost(const VtaConfig& config);
    ~EngineHost();

    // 确保语义引擎（RaNER + gte）已加载。单飞（single-flight）：并发调用共享同一次加载。
    // 返回 false = 加载失败（err 带原因）。
    bool EnsureEngines(const std::function<void(const std::string&)>& progress, std::string* err);

    // 确保 ASR 识别器已加载（常驻；无热词。hotwordsFile 参数保留仅为兼容旧调用，恒忽略）。
    bool EnsureAsr(const std::string& hotwordsFile, std::string* err);
    // 强制重建识别器（如切换 ASR 模型/需要复位解码器时）。
    // 旧识别器由仍在跑的会话共享持有，结束后自动释放，不会让在跑的识别流悬空。
    bool RebuildAsr(std::string* err);

    // 标记刚被使用（推迟空闲卸载）。
    void Touch();

    // GTCRN 运行时开关（方案 §5.2；对新会话生效）
    void SetDenoiseEnabled(bool enabled) { denoiseEnabled_ = enabled; }

    // 已加载的引擎（未加载返回 nullptr；调用方应先 EnsureEngines）
    RaNerEngine* Raner() const { return raner_.get(); }
    EmbeddingEngine* Embed() const { return embed_.get(); }
    // ASR 识别器为共享所有权：热词变更会整体重建，在跑的会话持有旧识别器
    // 直到自己结束才释放（否则其识别流会悬空）。
    std::shared_ptr<AsrRecognizer> Asr() const;
    GtcrnDenoiser* NewDenoiser(std::string* err) const;  // 每会话一个实例（模型共享）

    bool IsLoaded() const { return loaded_; }

    // 语音会话位簿记
    void OnSessionOpened();
    void OnSessionClosed();

private:
    void IdleLoop();
    bool BuildAsrLocked(std::string* err);  // 须持 gate_

    VtaConfig config_;
    std::mutex gate_;
    std::unique_ptr<RaNerEngine> raner_;
    std::unique_ptr<EmbeddingEngine> embed_;
    // ASR 识别器：共享所有权 + 独立锁（重建不影响在跑会话）
    mutable std::mutex asrMu_;
    std::shared_ptr<AsrRecognizer> asr_;
    bool loading_ = false;
    std::atomic<bool> loaded_{false};

    std::atomic<int64_t> lastTouchMs_;
    std::atomic<int> activeSessions_{0};
    std::atomic<bool> denoiseEnabled_{false};
    std::thread idleThread_;
    std::atomic<bool> quit_{false};
    // 空闲线程的等待/唤醒（析构时立即唤醒并 join，避免访问已销毁成员）
    std::mutex idleMu_;
    std::condition_variable idleCv_;
};

}  // namespace vta

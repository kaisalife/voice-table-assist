// jni/session/interaction_session.h —— 翻译自 Asr/VoiceInteractionSession.cs
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../text/triple_extractor.h"

namespace vta {

class RaNerEngine;
class EmbeddingEngine;
class VtxIndex;
class SoundAligner;

// 一次「说完了」的解析结果（AIDL CellHit 的内部形态；row/col 为向量库 1-based 原始坐标）
struct CellHitInternal {
    int row = 0;
    int column = 0;
    std::string value;      // 归一后的字符串，如 "17.84"
    std::string raw;        // 原始 ASR 文本片段（"十七点八四"）
    int64_t finalizedAtMs = 0;
};

// 一次提交的诊断反馈（解决"听到了却毫无反应"的体验问题）：
// 把 Submit 内部每一步的结果（NER 抽了几个三元组、每个三元组检索到的相似度、
// 是否低于 minSim 被丢弃）回传给 UI，让用户知道系统听到了、卡在哪一步。
struct SubmitFeedback {
    int64_t atMs = 0;
    std::string heard;        // 纠错后的累计文本
    int tripleCount = 0;      // NER 抽出的三元组数
    int hitCount = 0;         // 实际产出 cells 数
    std::string summary;      // 人类可读结果（多行，UI 直接显示）
};

// 每路语音会话一个的服务端交互编排器：
// 累积流式 final 文本 → 静默超时自动提交 → 进程内 RaNER + 向量检索 → 产出 cells（pollCells 拉取）。
// 提交后做"累计精简"：把 NER 抽回的最后一个完整三元组反拼回字符串作为下一轮前缀（原始累计清空）。
// 超 MaxChars 时整体清空重录。
class VoiceInteractionSession {
public:
    // raner/embed 引擎与活动索引由宿主注入（懒加载语义：openSession 前必须 EnsureEngines）；
    // aligner 为表内读音对齐器（可空）：在 NER 之前把听错的词吸附回本表规范写法。
    // index 为共享所有权：多表并发时本会话的索引不会被其他会话切表释放（不悬空）。
    VoiceInteractionSession(int silenceMs, int maxChars, float minSim, RaNerEngine* raner,
                            EmbeddingEngine* embed, std::shared_ptr<const VtxIndex> index,
                            std::shared_ptr<const SoundAligner> aligner = nullptr);
    ~VoiceInteractionSession();

    // 当前累计文本（线程安全快照；提交/溢出清空后为空串）
    std::string Accumulated() const;

    // 由识别循环在收到 final 识别片段时调用；自动重启静默计时。
    // 返回合并后的最新累计文本（溢出清空返回空串）。
    std::string OnFinal(const std::string& text);

    // 立即提交累积文本（closeSession 收尾时调用）。
    void Flush();

    // 把未进入累计的最新一段 partial 文本并入累计（不触发静默计时器、不触发超限清空）。
    // 用于"点结束会话"时收尾：partial 只下发 UI、不进累计，直接 Flush 会因累计为空而漏发 cells。
    void FoldPartial(const std::string& text);

    // cells 队列：阻塞拉取累积的 cells（等价 AIDL pollCells）。
    // 有货立即返回；无货最多等 timeoutMs（250ms）返回空。
    std::vector<CellHitInternal> PollCells(int maxN, int timeoutMs);

    // 提交诊断队列：每次 Submit 都产出一条（无论是否命中），供 UI 反馈"听到了但没匹配上"。
    // 语义同 PollCells：有货立即返回，无货最多等 timeoutMs。
    std::vector<SubmitFeedback> PollFeedback(int maxN, int timeoutMs);

    // 最近一条错误（解析失败等，供 health/诊断）；空 = 无
    std::string LastError() const;

    // 与验证页一致的流式文本合并（前缀扩展 / 包含去重 / 重叠拼接）。
    static std::string MergeText(const std::string& prev, const std::string& cur);

private:
    void RestartTimer();
    void TimerLoop();
    void CancelTimer();
    void Submit();
    void PushCells(std::vector<CellHitInternal> cells);
    void PushFeedback(SubmitFeedback fb);

    int silenceMs_;
    int maxChars_;
    RaNerEngine* raner_;
    EmbeddingEngine* embed_;
    std::shared_ptr<const VtxIndex> index_;  // 共享所有权：多表并发不悬空
    float minSim_;
    std::shared_ptr<const SoundAligner> aligner_;

    mutable std::mutex gate_;
    std::string accumulated_;

    // 静默自动提交计时器（单常驻线程 + 代次守卫；析构时 join，避免访问已销毁的 mutex）
    std::mutex timerMu_;
    std::condition_variable timerCv_;
    std::chrono::steady_clock::time_point timerDeadline_;
    std::thread timerThread_;
    uint64_t timerGeneration_ = 0;
    bool timerCancelled_ = true;
    bool timerStarted_ = false;

    mutable std::mutex cellsMu_;
    std::condition_variable cellsCv_;
    std::vector<CellHitInternal> cells_;

    // 提交诊断队列（与 cells 同语义，独立队列避免互相阻塞）
    mutable std::mutex feedbackMu_;
    std::condition_variable feedbackCv_;
    std::vector<SubmitFeedback> feedback_;

    std::atomic<bool> stopped_{false};
    std::string lastError_;
};

}  // namespace vta

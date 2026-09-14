// jni/session/interaction_session.cc —— 翻译自 Asr/VoiceInteractionSession.cs
#include "interaction_session.h"

#include <algorithm>

#include "../common/log.h"
#include "../common/strings.h"
#include "../embed/embedder.h"
#include "../ner/raner_engine.h"
#include "../tables/table_vector_manager.h"
#include "../text/chinese_numeral.h"
#include "../text/merge_text.h"
#include "../text/sound_aligner.h"

namespace vta {

VoiceInteractionSession::VoiceInteractionSession(int silenceMs, int maxChars, float minSim,
                                                 RaNerEngine* raner, EmbeddingEngine* embed,
                                                 std::shared_ptr<const VtxIndex> index,
                                                 std::shared_ptr<const SoundAligner> aligner)
    : silenceMs_(silenceMs), maxChars_(maxChars), raner_(raner), embed_(embed),
      index_(std::move(index)), minSim_(minSim), aligner_(std::move(aligner)) {
    embed_->minSim = minSim_;
}

VoiceInteractionSession::~VoiceInteractionSession() {
    {
        std::lock_guard<std::mutex> lk(timerMu_);
        stopped_ = true;
        timerCancelled_ = true;
    }
    timerCv_.notify_all();
    // 必须 join：计时线程持有 timerMu_/timerCv_，detach 会在对象析构后访问已销毁的 mutex
    if (timerThread_.joinable()) timerThread_.join();
}

std::string VoiceInteractionSession::Accumulated() const {
    std::lock_guard<std::mutex> lk(gate_);
    return accumulated_;
}

std::string VoiceInteractionSession::OnFinal(const std::string& textIn) {
    std::string text = Trim(textIn);
    if (text.empty()) return Accumulated();

    bool overflow = false;
    std::string merged;
    {
        std::lock_guard<std::mutex> lk(gate_);
        merged = MergeText(accumulated_, text);
        if (static_cast<int>(merged.size()) > maxChars_) {
            // 累积过多（如用户一直没停顿）：丢弃本次累积，防止把无关内容一并解析
            accumulated_.clear();
            overflow = true;
        } else {
            accumulated_ = merged;
        }
    }

    if (overflow) {
        CancelTimer();
        std::string message =
            "语音累计超过 " + std::to_string(maxChars_) + " 字，已自动清空，请分次录入";
        ALOGI("累积溢出清空: 上限=%d", maxChars_);
        // 端侧无 error 事件推送通道：记入 LastError，getState 轮询可见
        std::lock_guard<std::mutex> lk(gate_);
        lastError_ = "ACCUM_OVERFLOW: " + message;
        return "";
    }

    RestartTimer();
    return merged;
}

void VoiceInteractionSession::Flush() {
    CancelTimer();
    Submit();
}

void VoiceInteractionSession::FoldPartial(const std::string& text) {
    std::string trimmed = Trim(text);
    if (trimmed.empty()) return;
    std::lock_guard<std::mutex> lk(gate_);
    accumulated_ = MergeText(accumulated_, trimmed);
}

std::vector<CellHitInternal> VoiceInteractionSession::PollCells(int maxN, int timeoutMs) {
    std::unique_lock<std::mutex> lk(cellsMu_);
    if (cells_.empty()) {
        cellsCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                          [&] { return !cells_.empty(); });
    }
    size_t take = std::min(static_cast<size_t>(maxN > 0 ? maxN : 1), cells_.size());
    std::vector<CellHitInternal> out(cells_.begin(), cells_.begin() + static_cast<long>(take));
    cells_.erase(cells_.begin(), cells_.begin() + static_cast<long>(take));
    return out;
}

std::string VoiceInteractionSession::LastError() const {
    std::lock_guard<std::mutex> lk(gate_);
    return lastError_;
}

// ---- 私有实现 ----

// 单常驻计时线程：RestartTimer 只推进 deadline/generation，不新建线程。
// 线程对象由 timerThread_ 持有并在析构时 join（不可 detach：线程持有本对象的 mutex/cv）。
void VoiceInteractionSession::RestartTimer() {
    {
        std::lock_guard<std::mutex> lk(timerMu_);
        if (stopped_.load()) return;
        timerDeadline_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(silenceMs_);
        ++timerGeneration_;
        timerCancelled_ = false;
        if (!timerStarted_) {
            timerStarted_ = true;
            timerThread_ = std::thread([this] { TimerLoop(); });
        }
    }
    timerCv_.notify_all();
}

void VoiceInteractionSession::TimerLoop() {
    std::unique_lock<std::mutex> lk(timerMu_);
    while (!stopped_.load()) {
        if (timerGeneration_ == 0 || timerCancelled_) {
            timerCv_.wait(lk, [&] { return stopped_.load() || (!timerCancelled_ && timerGeneration_ > 0); });
            if (stopped_.load()) return;
        }
        auto deadline = timerDeadline_;  // 本地拷贝：等待期间 RestartTimer 可能改写成员
        if (timerCv_.wait_until(lk, deadline, [&] {
                return stopped_.load() || timerCancelled_;
            }))
            continue;  // 被取消/重启：回到等待
        if (stopped_.load()) return;
        lk.unlock();
        Submit();
        lk.lock();
        timerCancelled_ = true;  // 一次性：提交后停表，等下一次 OnFinal 重启
    }
}

void VoiceInteractionSession::CancelTimer() {
    {
        std::lock_guard<std::mutex> lk(timerMu_);
        timerCancelled_ = true;  // 令回调里的代次比对失效，防止已取消的计划仍提交
    }
    timerCv_.notify_all();
}

void VoiceInteractionSession::Submit() {
    // 提交后保留累计文本（不清空）：后续语句常省略行标签（如「二号是六十」），
    // 需要依赖前文主体才能正确解析；重复的格子会以相同/更新的值再填一遍，无害。
    std::string text;
    {
        std::lock_guard<std::mutex> lk(gate_);
        text = Trim(accumulated_);
    }
    if (text.empty()) return;

    SubmitFeedback fb;
    fb.atMs = NowMs();
    fb.heard = text;

    // 表内读音吸附（在 NER 之前）：
    // RaNER 是用正确行名训练的，先按读音把听错的片段吸附回本表规范写法，
    // NER 才抽得出主体/位置。词表 = 本表行标签 + 列说法（自动），零人工维护。
    if (aligner_ && aligner_->Ready()) {
        std::string aligned = aligner_->AlignSentence(text);
        if (aligned != text) {
            ALOGI("[ALIGN] 提交文本吸附: \"%s\" → \"%s\"", text.c_str(), aligned.c_str());
            text = aligned;
        }
    }

    try {
        auto t0 = std::chrono::steady_clock::now();
        auto bio = raner_->Predict(text);
        auto triples = ExtractTriples(bio);
        auto t1 = std::chrono::steady_clock::now();
        fb.tripleCount = static_cast<int>(triples.size());
        if (!triples.empty())
            ALOGI("[SUBMIT] '%s' -> %zu 个三元组", text.c_str(), triples.size());
        double lookupMs = 0;
        auto msf = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };

        std::vector<CellHitInternal> hits;
        std::string detail;  // 每个三元组的命中/未命中细节
        for (const auto& t : triples) {
            // 检索 "主体+位置"（如「水位一号」）→ 表内行列；sim 与最近邻短语无论是否命中都拿到
            auto lk0 = std::chrono::steady_clock::now();
            auto hit = embed_->Lookup(t.sub + t.obj, index_.get());
            lookupMs += msf(lk0, std::chrono::steady_clock::now());
            std::string value = FormatCellValue(ChineseNumeralToDecimal(t.val));
            char buf[256];
            if (hit.row <= 0) {
                // 低置信：宁缺勿错填（minSim 过滤）。这里把"差多少"告诉用户，而不是静默丢弃
                std::snprintf(buf, sizeof(buf), "  · “%s%s = %s” 未匹配（最接近“%s”，相似度 %.2f < %.2f）",
                              t.sub.c_str(), t.obj.c_str(), t.val.c_str(), hit.phrase.c_str(),
                              hit.sim, minSim_);
                detail += buf;
                ALOGI("[SUBMIT] 未命中: sub=%s obj=%s val=%s best=%s sim=%.3f (minSim=%.2f)",
                      t.sub.c_str(), t.obj.c_str(), t.val.c_str(), hit.phrase.c_str(), hit.sim,
                      minSim_);
                continue;
            }
            CellHitInternal c;
            c.row = hit.row;
            c.column = hit.col;
            c.value = value;
            c.raw = t.val;
            c.finalizedAtMs = NowMs();
            hits.push_back(std::move(c));
            std::snprintf(buf, sizeof(buf), "  · “%s%s = %s” → 第%d行 第%d列（相似度 %.2f）",
                          t.sub.c_str(), t.obj.c_str(), t.val.c_str(), hit.row, hit.col, hit.sim);
            detail += buf;
        }
        fb.hitCount = static_cast<int>(hits.size());
        ALOGI("[PERF] SUBMIT: 文本 %zu 字, NER %.0fms, 检索 %.0fms (%zu 项)", text.size(), msf(t0, t1),
              lookupMs, triples.size());

        // ---- 组装人类可读结论（UI 直接显示，不用再解析）----
        std::string s = "听到：“" + text + "”";
            if (fb.tripleCount == 0) {
                // 表外语/结构不完整：再查一次"整句与表内最接近的短语"，给用户一个参照
                auto near = embed_->Lookup(text, index_.get());
            char buf[256];
            if (near.row > 0) {
                std::snprintf(buf, sizeof(buf), "\n未匹配到表格项：没听出「主体 + 位置 + 数值」结构"
                                                "（表内最接近“%s”，相似度 %.2f）",
                              near.phrase.c_str(), near.sim);
            } else {
                std::snprintf(buf, sizeof(buf), "\n未匹配到表格项：没听出「主体 + 位置 + 数值」结构"
                                                "（表内最接近“%s”，相似度 %.2f < %.2f）",
                              near.phrase.c_str(), near.sim, minSim_);
            }
            s += buf;
        } else if (fb.hitCount == 0) {
            s += "\n未匹配到表格项（" + std::to_string(fb.tripleCount) + " 项全部低于阈值）:" + detail;
        } else {
            s += "\n已填入 " + std::to_string(fb.hitCount) + "/" + std::to_string(fb.tripleCount) +
                 " 项:" + detail;
        }
        fb.summary = s;
        ALOGI("[SUBMIT] %s", fb.summary.c_str());
        PushFeedback(std::move(fb));
        PushCells(std::move(hits));

        // 累计精简：把"最后一个完整三元组 (Sub,Obj,Val)"反拼回字符串作为下一轮前缀，
        // 原始累计清空。守卫：① NER 抽回 0 个三元组 → 保留原累计；
        // ② 末尾三元组含占位 "?"（OBJ/VAL 缺一）→ 保留原累计；③ Sub 为空 → 同样保留。
        if (!triples.empty()) {
            const Triple& last = triples.back();
            if (!last.sub.empty() && last.obj != "?" && last.val != "?" && !last.obj.empty() &&
                !last.val.empty()) {
                std::string prefix = Trim(last.sub + last.obj + last.val);
                if (!prefix.empty()) {
                    std::lock_guard<std::mutex> lk(gate_);
                    accumulated_ = prefix;
                    lastError_.clear();
                    ALOGI("累计精简: \"%s\" -> \"%s\"", text.c_str(), prefix.c_str());
                }
            }
        }
    } catch (const std::exception& ex) {
        ALOGW("语音交互解析失败: %s: %s", text.c_str(), ex.what());
        {
            std::lock_guard<std::mutex> lk(gate_);
            lastError_ = std::string("PARSE_FAILED: 解析失败：") + ex.what();
        }
        fb.summary = "听到：“" + text + "”\n解析失败：" + ex.what();
        PushFeedback(std::move(fb));
    }
}

void VoiceInteractionSession::PushCells(std::vector<CellHitInternal> cells) {
    if (cells.empty()) return;
    {
        std::lock_guard<std::mutex> lk(cellsMu_);
        for (auto& c : cells) cells_.push_back(std::move(c));
    }
    cellsCv_.notify_all();
}

void VoiceInteractionSession::PushFeedback(SubmitFeedback fb) {
    {
        std::lock_guard<std::mutex> lk(feedbackMu_);
        feedback_.push_back(std::move(fb));
    }
    feedbackCv_.notify_all();
}

std::vector<SubmitFeedback> VoiceInteractionSession::PollFeedback(int maxN, int timeoutMs) {
    std::unique_lock<std::mutex> lk(feedbackMu_);
    if (feedback_.empty() && timeoutMs > 0) {
        feedbackCv_.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                             [&] { return !feedback_.empty(); });
    }
    size_t take = std::min(static_cast<size_t>(maxN > 0 ? maxN : 1), feedback_.size());
    std::vector<SubmitFeedback> out(feedback_.begin(), feedback_.begin() + static_cast<long>(take));
    feedback_.erase(feedback_.begin(), feedback_.begin() + static_cast<long>(take));
    return out;
}

std::string VoiceInteractionSession::MergeText(const std::string& prev, const std::string& cur) {
    return MergeStreamingText(prev, cur);
}

}  // namespace vta

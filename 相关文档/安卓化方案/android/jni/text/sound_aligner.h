// jni/text/sound_aligner.h —— 表内读音对齐器（新语音输入同音解决方案）
//
// 目标：把 ASR 听错的"主体/位置"按【读音】吸附到当前表的词表上，零人工维护。
//   词表来源（全部自动）：行标签（表本身） + 列说法（由列数枚举）
//   音近模型（与表无关的通用规则，硬编码一次）：
//     忽略声调 / 前鼻音后鼻音不分 / 平舌翘舌不分 / n-l、f-h 不分 / 音节编辑距离≤1
//   防误纠守卫：相似度阈值 + 与次优候选的差距 + 音节数一致 → 分不清就放弃（宁缺勿错填）
//
// 详见 相关文档/新语音输入同音解决方案.md
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "../common/strings.h"

namespace vta {

class PinyinTable;

class SoundAligner {
public:
    // rowLabels：本表行标签；columnCount：列数（用于枚举位置词表）
    SoundAligner(std::shared_ptr<const PinyinTable> pinyin, std::vector<std::string> rowLabels,
                 int columnCount);

    bool Ready() const { return pinyin_ != nullptr && !words_.empty(); }
    const std::vector<std::string>& Vocabulary() const { return vocabText_; }

    struct Match {
        std::string canonical;  // 规范写法；空 = 未采纳
        double score = 0.0;     // 相似度
        double margin = 0.0;    // 与次优候选的差距
    };

    // 整词吸附：在词表里找读音最像的词；不达阈值/分不清 → canonical 为空
    Match AlignWord(const std::string& word) const;

    // 整句吸附：把句中高置信的片段替换为词表规范写法，其余原样保留。
    // 用于喂 NER 之前：RaNER 用正确行名训练，先把错字纠回正字，NER 才抽得出主体。
    std::string AlignSentence(const std::string& text) const;

    // 调参（单测/现场可调）；默认 accept=0.75、margin=0.15
    void SetThresholds(double accept, double margin) {
        thr_ = accept;
        margin_ = margin;
    }

    // 音节相似度（供单测）
    static double SyllableSim(const std::string& a, const std::string& b);
    static double SequenceSim(const std::vector<std::string>& a, const std::vector<std::string>& b);

private:
    struct Entry {
        std::string text;
        std::vector<CodePoint> cps;          // 码点（保护区判定用）
        std::vector<std::string> syllables;
    };

    // 命中前二名（best/second），用于阈值 + 差距守卫
    void TopTwo(const std::vector<std::string>& spanSyl, std::string* bestText, double* bestScore,
                double* secondScore) const;

    std::shared_ptr<const PinyinTable> pinyin_;
    std::vector<Entry> words_;
    std::vector<std::string> vocabText_;
    double thr_ = 0.75;
    double margin_ = 0.15;
};

}  // namespace vta

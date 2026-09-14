// jni/ner/raner_engine.h —— 翻译自 Services/RaNerEngine.cs + models/raner/export-int8/test_int8.py
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../common/strings.h"

namespace vta {

// RaNER（BERT+CRF NER）引擎：
//  int8 量化模型（models/raner/export-int8/model.onnx，onnxruntime 动态量化 QInt8 per-channel，
//  388MB→103MB，emissions 余弦相似度 0.9989），逐字 tokenizer（vocab.txt），MAX_LEN=128，
//  输出 emissions 接 Viterbi 得 BIO 标签。
//  CRF 解码与 torchcrf._viterbi_decode 逐位对齐（含 start/end 转移，test_int8.py 语义）：
//    score = start_transitions + emissions[0]
//    score[t,j] = max_i(score[t-1,i] + transitions[i,j]) + emissions[t,j]
//    终局 score += end_transitions，回溯取最优路径
//  兼容旧 fp32 单文件 crf_transitions.json（无 start/end 时置零）。
class RaNerEngine {
public:
    static constexpr int kMaxLen = 128;
    static constexpr CodePoint kCls = 101, kSep = 102, kPad = 0, kUnk = 100;
    static const char* const kLabels[7];

    explicit RaNerEngine(const std::string& modelDir);
    ~RaNerEngine();
    RaNerEngine(const RaNerEngine&) = delete;
    RaNerEngine& operator=(const RaNerEngine&) = delete;

    // 返回 (字符, 标签) 序列（word 级标签折叠回字符）
    std::vector<std::pair<std::string, std::string>> Predict(const std::string& text);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    std::unordered_map<std::string, int> _vocab;
    std::vector<float> _transitions;        // [7*7] 平铺 CRF 转移矩阵 [from,to]
    std::vector<float> _startTransitions;   // [7] 起始转移（旧格式无则全 0）
    std::vector<float> _endTransitions;     // [7] 结束转移（旧格式无则全 0）

    void LoadVocab(const std::string& path);
    // 优先 export-int8 的 .npy（torchcrf 完整语义）；回退旧 crf_transitions.json
    void LoadCrfParams(const std::string& modelDir);
};

}  // namespace vta

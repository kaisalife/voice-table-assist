// jni/embed/embedder.h —— 翻译自 Services/EmbeddingEngine.cs + BerTokenizer.cs
#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "vtx1.h"

namespace vta {

// gte-base-zh 嵌入引擎：
//  1. 加载 model_quantized.onnx + tokenizer.json（线程安全、进程内共享、不随表重建）；
//  2. Query(phrase) -> attention-mask 加权 mean 池化 + normalize -> 与库向量余弦 -> 最近邻 (row,col)
//     （gte 系模型官方推荐 mean 池化；池化方式变更后向量库需重建）。
class EmbeddingEngine {
public:
    explicit EmbeddingEngine(const std::string& modelDir);
    ~EmbeddingEngine();
    EmbeddingEngine(const EmbeddingEngine&) = delete;
    EmbeddingEngine& operator=(const EmbeddingEngine&) = delete;

    // 最近邻余弦相似度低于该值视为未命中（返回 row=-1）；0=不过滤（等价 Embedding:MinSim）。
    float minSim = 0.55f;

    std::vector<float> Embed(const std::string& text);
    // 批量嵌入：将多条文本在一次 ONNX 推理中完成。
    std::vector<std::vector<float>> EmbedBatch(const std::vector<std::string>& texts, int maxLen = 256);

    struct LookupResult {
        int row = -1;
        int col = -1;
        float sim = 0.f;
        std::string phrase;
    };

    // 按指定表的索引查询（会话级快照）。低置信返回 row=-1（宁缺勿错填）。
    LookupResult Lookup(const std::string& phrase, const VtxIndex* index) const;

    // 与 mjs embedQuery 一致：去掉中文/半角逗号与空白。
    static std::string CleanPhrase(const std::string& phrase);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::unordered_map<std::string, int> _vocab;
};

}  // namespace vta

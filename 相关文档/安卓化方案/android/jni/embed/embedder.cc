// jni/embed/embedder.cc —— 翻译自 Services/EmbeddingEngine.cs + BerTokenizer.cs
// gte-base-zh = BERT tokenizer（do_lower_case + tokenize_chinese_chars），
// 直接以 tokenizer.json 的 model.vocab 建词表，复刻 BERT basic+wordpiece。
#include "embedder.h"

#include <cmath>
#include <limits>
#include <stdexcept>
#include <onnxruntime_cxx_api.h>

#include "../common/json.h"
#include "../common/log.h"
#include "../common/strings.h"

namespace vta {

namespace {
constexpr int kCls = 101, kSep = 102, kPad = 0, kUnk = 100;

inline bool IsCjkBasic(CodePoint cp) { return cp >= 0x4E00 && cp <= 0x9FA5; }

// BERT basic tokenize：CJK 字符两侧加空格切分 + ASCII 小写 + 按空白切
std::vector<std::string> BasicTokenize(const std::string& text) {
    std::string spaced;
    for (CodePoint cp : Utf8ToCodePoints(text)) {
        if (IsCjkBasic(cp)) {
            spaced += ' ';
            AppendUtf8(cp, &spaced);
            spaced += ' ';
        } else {
            AppendUtf8(cp, &spaced);
        }
    }
    std::vector<std::string> out;
    std::string cur;
    for (char c : spaced) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (!cur.empty()) out.push_back(std::move(cur));
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    for (auto& w : out) w = ToLowerAscii(w);
    return out;
}

// BERT wordpiece：## 前缀贪心最长匹配；无解产出 [UNK] 并前进一字
std::vector<int> WordPiece(const std::unordered_map<std::string, int>& vocab,
                           const std::string& word) {
    std::vector<int> out;
    auto cps = Utf8ToCodePoints(word);
    if (cps.empty()) return out;
    auto cpsToUtf8 = [](const std::vector<CodePoint>& v, size_t b, size_t e) {
        std::string s;
        for (size_t i = b; i < e; ++i) AppendUtf8(v[i], &s);
        return s;
    };
    {
        auto it = vocab.find(word);
        if (it != vocab.end()) { out.push_back(it->second); return out; }
    }
    size_t start = 0;
    while (start < cps.size()) {
        size_t end = cps.size();
        int found = -1;
        while (end > start) {
            std::string sub = (start > 0 ? "##" : "") + cpsToUtf8(cps, start, end);
            auto it = vocab.find(sub);
            if (it != vocab.end()) { found = it->second; break; }
            --end;
        }
        if (found >= 0) {
            out.push_back(found);
            start = end;
        } else {
            out.push_back(kUnk);
            ++start;
        }
    }
    return out;
}

std::vector<float> Normalize(std::vector<float> v) {
    double s = 0;
    for (float x : v) s += static_cast<double>(x) * x;
    double n = std::sqrt(s);
    if (n > 0)
        for (auto& x : v) x = static_cast<float>(x / n);
    return v;
}

}  // namespace

struct EmbeddingEngine::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "vta-embed"};
    std::unique_ptr<Ort::Session> session;
    std::vector<std::string> inputNames;
    std::string lastHiddenName;
    size_t lastHiddenIndex = 0;
};

EmbeddingEngine::EmbeddingEngine(const std::string& modelDir) : impl_(new Impl()) {
    // tokenizer.json：只取 model.vocab
    auto doc = json::ParseFile(modelDir + "/tokenizer.json");
    if (!doc) throw std::runtime_error("cannot load tokenizer.json: " + modelDir);
    const auto& vocabNode = doc->at("model").at("vocab");
    if (!vocabNode.isObject()) throw std::runtime_error("tokenizer.json: model.vocab missing");
    for (const auto& [tok, id] : vocabNode.asObject()) _vocab.emplace(tok, id.asInt());

    Ort::SessionOptions opts;  // 与现网 .NET 一致：ORT 默认线程数/优化级别
    impl_->session =
        std::make_unique<Ort::Session>(impl_->env, (modelDir + "/model_quantized.onnx").c_str(), opts);

    Ort::AllocatorWithDefaultOptions alloc;
    size_t n = impl_->session->GetInputCount();
    for (size_t i = 0; i < n; ++i) {
        auto name = impl_->session->GetInputNameAllocated(i, alloc);
        impl_->inputNames.emplace_back(name.get());
    }
    size_t m = impl_->session->GetOutputCount();
    std::vector<std::string> outputNames;
    for (size_t i = 0; i < m; ++i) {
        auto name = impl_->session->GetOutputNameAllocated(i, alloc);
        outputNames.emplace_back(name.get());
    }
    size_t idx = 0;
    for (size_t i = 0; i < outputNames.size(); ++i) {
        if (outputNames[i].find("last_hidden_state") != std::string::npos) { idx = i; break; }
    }
    impl_->lastHiddenName = outputNames[idx];
    impl_->lastHiddenIndex = idx;
    ALOGI("[EMBED] model loaded: %s (vocab=%zu)", modelDir.c_str(), _vocab.size());
}

EmbeddingEngine::~EmbeddingEngine() = default;

std::vector<int64_t> EncodeTokens(const std::unordered_map<std::string, int>& vocab,
                                  const std::string& text, int maxLen, std::vector<int64_t>* maskOut) {
    std::vector<int64_t> tokens{kCls};
    for (const auto& word : BasicTokenize(text)) {
        auto it = vocab.find(word);
        if (it != vocab.end()) {
            tokens.push_back(it->second);
            continue;
        }
        for (int piece : WordPiece(vocab, word)) tokens.push_back(piece);
    }
    tokens.push_back(kSep);
    if (static_cast<int>(tokens.size()) > maxLen - 1) {
        tokens.resize(maxLen - 1);
        tokens.push_back(kSep);
    }

    std::vector<int64_t> ids(maxLen, 0), mask(maxLen, 0);
    for (size_t i = 0; i < tokens.size(); ++i) {
        ids[i] = tokens[i];
        mask[i] = 1;
    }
    if (maskOut) *maskOut = mask;
    return ids;
}

std::vector<float> EmbeddingEngine::Embed(const std::string& text) {
    auto batch = EmbedBatch({text}, 256);
    return std::move(batch[0]);
}

std::vector<std::vector<float>> EmbeddingEngine::EmbedBatch(const std::vector<std::string>& texts,
                                                            int maxLen) {
    if (texts.empty()) return {};

    // 1. 逐条编码，记录最大长度（实际非 padding 长度）
    struct Enc {
        std::vector<int64_t> ids, mask, types;
        int len;
    };
    std::vector<Enc> encoded;
    encoded.reserve(texts.size());
    int maxSeq = 0;
    for (const auto& text : texts) {
        Enc e;
        e.ids = EncodeTokens(_vocab, text, maxLen, &e.mask);
        e.types.assign(maxLen, 0);
        e.len = 0;
        for (int64_t id : e.ids)
            if (id != 0) ++e.len;
        if (e.len > maxSeq) maxSeq = e.len;
        encoded.push_back(std::move(e));
    }
    if (maxSeq > maxLen) maxSeq = maxLen;
    if (maxSeq < 1) maxSeq = 1;

    // 2. 构建 batch 张量 [batch_size, maxSeq]
    int batchSize = static_cast<int>(texts.size());
    std::vector<int64_t> idsBatch(batchSize * maxSeq, 0);
    std::vector<int64_t> maskBatch(batchSize * maxSeq, 0);
    std::vector<int64_t> typesBatch(batchSize * maxSeq, 0);
    for (int b = 0; b < batchSize; ++b) {
        for (int s = 0; s < maxSeq; ++s) {
            int idx = b * maxSeq + s;
            idsBatch[idx] = s < static_cast<int>(encoded[b].ids.size()) ? encoded[b].ids[s] : 0;
            maskBatch[idx] = s < static_cast<int>(encoded[b].mask.size()) ? encoded[b].mask[s] : 0;
            typesBatch[idx] = s < static_cast<int>(encoded[b].types.size()) ? encoded[b].types[s] : 0;
        }
    }

    // 3. 运行 ONNX
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<int64_t> shape{static_cast<int64_t>(batchSize), static_cast<int64_t>(maxSeq)};
    Ort::Value idsT = Ort::Value::CreateTensor<int64_t>(mem, idsBatch.data(), idsBatch.size(), shape.data(), 2);
    Ort::Value maskT = Ort::Value::CreateTensor<int64_t>(mem, maskBatch.data(), maskBatch.size(), shape.data(), 2);
    Ort::Value typesT = Ort::Value::CreateTensor<int64_t>(mem, typesBatch.data(), typesBatch.size(), shape.data(), 2);

    std::vector<const char*> inNames;
    std::vector<Ort::Value> inVals;
    for (const auto& n : impl_->inputNames) {
        if (n == "input_ids") { inNames.push_back(n.c_str()); inVals.push_back(std::move(idsT)); }
        else if (n == "attention_mask") { inNames.push_back(n.c_str()); inVals.push_back(std::move(maskT)); }
        else if (n == "token_type_ids") { inNames.push_back(n.c_str()); inVals.push_back(std::move(typesT)); }
    }

    // 显式请求 last_hidden_state 输出（新版 ORT 不允许 0 输出）
    const char* outNames[] = {impl_->lastHiddenName.c_str()};
    auto outputs = impl_->session->Run(Ort::RunOptions{nullptr}, inNames.data(), inVals.data(),
                                       inVals.size(), outNames, 1);
    const Ort::Value& hidden = outputs[impl_->lastHiddenIndex];
    auto info = hidden.GetTensorTypeAndShapeInfo();
    auto dims = info.GetShape();  // [batch, seq, dim]
    const int64_t dim = dims[2];
    const float* data = hidden.GetTensorData<float>();

    // 4. 逐样本 mean 池化（attention-mask 加权）并归一化
    std::vector<std::vector<float>> results;
    results.reserve(batchSize);
    for (int b = 0; b < batchSize; ++b) {
        std::vector<float> vec(dim, 0.f);
        double denom = 0;
        for (int s = 0; s < maxSeq; ++s) {
            if (maskBatch[b * maxSeq + s] == 0) continue;
            denom += 1;
            const float* row = data + (static_cast<int64_t>(b) * maxSeq + s) * dim;
            for (int64_t d = 0; d < dim; ++d) vec[d] += row[d];
        }
        if (denom > 0)
            for (auto& x : vec) x = static_cast<float>(x / denom);
        results.push_back(Normalize(std::move(vec)));
    }
    return results;
}

std::string EmbeddingEngine::CleanPhrase(const std::string& phrase) {
    std::string sb;
    for (CodePoint cp : Utf8ToCodePoints(phrase)) {
        if (cp == 0xFF0C /*，*/ || cp == ',' || cp == 0x3001 /*、*/ || cp == ' ' || cp == '\t' ||
            cp == '\n' || cp == '\r')
            continue;
        AppendUtf8(cp, &sb);
    }
    return sb;
}

EmbeddingEngine::LookupResult EmbeddingEngine::Lookup(const std::string& phrase,
                                                      const VtxIndex* index) const {
    LookupResult res;
    if (index == nullptr || index->entries.empty()) return res;
    std::string clean = CleanPhrase(phrase);
    std::vector<float> q = const_cast<EmbeddingEngine*>(this)->Embed(clean);
    int dim = index->dim;
    const VtxCell* best = &index->entries[0];
    float bestSim = -std::numeric_limits<float>::infinity();
    for (const auto& e : index->entries) {
        float s = Vtx1Dot(q.data(), e.vec.data(), dim);
        if (s > bestSim) { bestSim = s; best = &e; }
    }
    res.sim = bestSim;
    res.phrase = best->phrase;
    if (bestSim < minSim) {  // 低置信：宁缺勿错填
        res.row = -1;
        res.col = -1;
        return res;
    }
    res.row = best->row;
    res.col = best->col;
    return res;
}

}  // namespace vta

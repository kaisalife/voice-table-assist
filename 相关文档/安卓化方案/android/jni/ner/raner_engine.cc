// jni/ner/raner_engine.cc —— 翻译自 Services/RaNerEngine.cs + export-int8/test_int8.py
#include "raner_engine.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>
#include <onnxruntime_cxx_api.h>

#include "../common/json.h"
#include "../common/log.h"

namespace vta {

const char* const RaNerEngine::kLabels[7] = {"O", "B-SUB", "I-SUB", "B-OBJ", "I-OBJ", "B-VAL", "I-VAL"};

struct RaNerEngine::Impl {
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "vta-raner"};
    std::unique_ptr<Ort::Session> session;
    std::vector<std::string> inputNames;
    std::string emissionsName;
    size_t emissionsIndex = 0;
};

// ---- .npy 读取（float32 一维/二维，v1.0 头）----
namespace {

bool LoadNpyFloat32(const std::string& path, std::vector<float>* out, std::vector<int64_t>* shape) {
    std::string blob;
    if (!ReadFileBytes(path, &blob)) return false;
    if (blob.size() < 10 || std::memcmp(blob.data(), "\x93NUMPY", 6) != 0) return false;
    uint8_t major = static_cast<uint8_t>(blob[6]);
    size_t off = 8;
    uint16_t headerLen = 0;
    uint32_t headerLen32 = 0;
    if (major == 1) {
        std::memcpy(&headerLen, blob.data() + off, 2);
        off += 2;
    } else {
        std::memcpy(&headerLen32, blob.data() + off, 4);
        off += 4;
        headerLen = static_cast<uint16_t>(headerLen32);
    }
    std::string header = blob.substr(off, headerLen);
    off += headerLen;
    // 解析 descr/shape（足够简单：descr 必须是 '<f4'，fortran_order 必须为 False）
    if (header.find("'float32'") == std::string::npos &&
        header.find("<f4") == std::string::npos)
        return false;
    if (header.find("False") == std::string::npos && header.find("false") == std::string::npos)
        return false;  // fortran_order 必须为 False
    // shape: ('shape' : (7, 7) ) —— 提取括号内数字
    size_t sp = header.find("'shape'");
    if (sp == std::string::npos) return false;
    size_t p1 = header.find('(', sp);
    size_t p2 = header.find(')', p1);
    if (p1 == std::string::npos || p2 == std::string::npos) return false;
    shape->clear();
    int64_t total = 1;
    std::string dims = header.substr(p1 + 1, p2 - p1 - 1);
    size_t i = 0;
    while (i < dims.size()) {
        while (i < dims.size() && !std::isdigit(static_cast<unsigned char>(dims[i]))) ++i;
        if (i >= dims.size()) break;
        int64_t v = 0;
        while (i < dims.size() && std::isdigit(static_cast<unsigned char>(dims[i]))) {
            v = v * 10 + (dims[i] - '0');
            ++i;
        }
        shape->push_back(v);
        total *= v;
    }
    size_t bytes = blob.size() - off;
    if (bytes < static_cast<size_t>(total) * sizeof(float)) return false;
    out->resize(total);
    std::memcpy(out->data(), blob.data() + off, static_cast<size_t>(total) * sizeof(float));
    return true;
}

}  // namespace

RaNerEngine::RaNerEngine(const std::string& modelDir) : impl_(new Impl()) {
    LoadVocab(modelDir + "/vocab.txt");
    LoadCrfParams(modelDir);

    Ort::SessionOptions opts;  // 与现网 .NET 一致：使用 ORT 默认线程数/优化级别，不做额外调参
    impl_->session = std::make_unique<Ort::Session>(impl_->env, (modelDir + "/model.onnx").c_str(), opts);

    Ort::AllocatorWithDefaultOptions alloc;
    size_t n = impl_->session->GetInputCount();
    for (size_t i = 0; i < n; ++i) {
        auto name = impl_->session->GetInputNameAllocated(i, alloc);
        impl_->inputNames.emplace_back(name.get());
    }
    // 与 test_int8.py 一致：emissions 取 outputs[0]（int8 导出仅一个输出）；名字仅作日志
    size_t m = impl_->session->GetOutputCount();
    std::vector<std::string> outputNames;
    for (size_t i = 0; i < m; ++i) {
        auto name = impl_->session->GetOutputNameAllocated(i, alloc);
        outputNames.emplace_back(name.get());
    }
    size_t idx = 0;
    for (size_t i = 0; i < outputNames.size(); ++i) {
        if (outputNames[i].find("emission") != std::string::npos) { idx = i; break; }
    }
    impl_->emissionsName = outputNames[idx];
    impl_->emissionsIndex = idx;
    ALOGI("[RANER] model loaded: %s (inputs=%zu, emissions=%s)", modelDir.c_str(), n,
          impl_->emissionsName.c_str());
}

RaNerEngine::~RaNerEngine() = default;

void RaNerEngine::LoadVocab(const std::string& path) {
    // 逐行 vocab：行号即 token id（首行 [PAD]=0，第 101 行 [UNK]=100，与 BERT vocab.txt 一致）
    auto lines = ReadFileLines(path);
    for (size_t i = 0; i < lines.size(); ++i) {
        std::string t = Trim(lines[i]);
        if (!t.empty()) _vocab.emplace(t, static_cast<int>(i));
    }
}

void RaNerEngine::LoadCrfParams(const std::string& modelDir) {
    // 优先 export-int8 的 .npy（torchcrf 完整语义：transitions + start + end）
    std::vector<int64_t> shape;
    if (LoadNpyFloat32(modelDir + "/crf_transitions.npy", &_transitions, &shape) &&
        LoadNpyFloat32(modelDir + "/crf_start_transitions.npy", &_startTransitions, &shape) &&
        LoadNpyFloat32(modelDir + "/crf_end_transitions.npy", &_endTransitions, &shape)) {
        if (_transitions.size() == 49 && _startTransitions.size() == 7 &&
            _endTransitions.size() == 7) {
            ALOGI("[RANER] CRF params from npy (torchcrf semantics: transitions+start+end)");
            return;
        }
        ALOGW("[RANER] npy 形状不符，回退 crf_transitions.json");
        _transitions.clear();
    }
    // 回退：旧 fp32 单文件（无 start/end，置零）
    _startTransitions.assign(7, 0.f);
    _endTransitions.assign(7, 0.f);
    auto doc = json::ParseFile(modelDir + "/crf_transitions.json");
    if (!doc) throw std::runtime_error("cannot load CRF params (npy/json) from: " + modelDir);
    const json::Value& root = *doc;
    if (root.isArray() && !root.asArray().empty() && root.asArray()[0].isNumber()) {
        for (const auto& e : root.asArray()) _transitions.push_back(static_cast<float>(e.asDouble()));
        return;
    }
    if (root.isArray()) {
        for (const auto& arr : root.asArray())
            for (const auto& x : arr.asArray()) _transitions.push_back(static_cast<float>(x.asDouble()));
        return;
    }
    for (const auto& e : root.at("data").asArray())
        _transitions.push_back(static_cast<float>(e.asDouble()));
}

namespace {

// 类常量的文件内别名（匿名命名空间辅助函数用）
constexpr int kMaxLen = RaNerEngine::kMaxLen;
constexpr CodePoint kCls = RaNerEngine::kCls;
constexpr CodePoint kSep = RaNerEngine::kSep;
constexpr CodePoint kPad = RaNerEngine::kPad;
constexpr CodePoint kUnk = RaNerEngine::kUnk;

// ---------- 逐字 tokenizer ----------
struct Tokenized {
    int64_t ids[kMaxLen] = {0};
    int64_t mask[kMaxLen] = {0};
    int wIdx[kMaxLen];
};

Tokenized Tokenize(const std::unordered_map<std::string, int>& vocab, const std::string& text) {
    auto cps = Utf8ToCodePoints(text);
    Tokenized t;
    for (int i = 0; i < kMaxLen; ++i) t.wIdx[i] = -1;

    std::vector<int64_t> ids{kCls};
    std::vector<int> wordIds{-1};
    for (size_t i = 0; i < cps.size(); ++i) {
        std::string ch = CodePointsToUtf8({cps[i]});
        auto it = vocab.find(ch);
        ids.push_back(it != vocab.end() ? it->second : static_cast<int64_t>(kUnk));
        wordIds.push_back(static_cast<int>(i));
    }
    ids.push_back(kSep);
    wordIds.push_back(-1);

    for (size_t i = 0; i < ids.size() && i < kMaxLen; ++i) {
        t.ids[i] = ids[i];
        t.mask[i] = 1;
        if (wordIds[i] >= 0) t.wIdx[i] = wordIds[i];
    }
    return t;
}

// ---------- Viterbi（torchcrf._viterbi_decode 逐位对齐，test_int8.py 语义）----------
// 返回长度 = MAX_LEN 的路径；seq_end 之后（padding 位）标签 = O(0)。
std::vector<int> Viterbi(const float* emissions, const int64_t* mask, int seqLen,
                         const float* transitions, const float* startTransitions,
                         const float* endTransitions, int numLabels) {
    // 有效长度 = mask 前导连续 1 的个数（padding 在尾部）；seq_end 为最后一个有效位下标
    int seqEnd = 0;
    while (seqEnd < seqLen && mask[seqEnd] != 0) ++seqEnd;
    --seqEnd;
    if (seqEnd < 0) return std::vector<int>(seqLen, 0);

    std::vector<float> score(numLabels);
    std::vector<std::vector<int>> history;  // history[t-1][j] = argmax_i

    // Start transition + first emission
    for (int j = 0; j < numLabels; ++j) score[j] = startTransitions[j] + emissions[j];

    for (int t = 1; t <= seqEnd; ++t) {
        std::vector<int> indices(numLabels);
        std::vector<float> next(numLabels);
        for (int j = 0; j < numLabels; ++j) {
            float best = -std::numeric_limits<float>::infinity();
            int bestI = 0;
            for (int i = 0; i < numLabels; ++i) {
                float s = score[i] + transitions[i * numLabels + j];
                if (s > best) { best = s; bestI = i; }
            }
            next[j] = best + emissions[t * numLabels + j];
            indices[j] = bestI;
        }
        score = std::move(next);
        history.push_back(std::move(indices));
    }

    // 终局：+ end_transitions
    int bestLast = 0;
    float bestScore = -std::numeric_limits<float>::infinity();
    for (int j = 0; j < numLabels; ++j) {
        float s = score[j] + endTransitions[j];
        if (s > bestScore) { bestScore = s; bestLast = j; }
    }

    std::vector<int> path(seqLen, 0);
    path[seqEnd] = bestLast;
    for (int t = seqEnd; t > 0; --t) path[t - 1] = history[t - 1][path[t]];
    return path;
}

}  // namespace

std::vector<std::pair<std::string, std::string>> RaNerEngine::Predict(const std::string& text) {
    Tokenized tok = Tokenize(_vocab, text);
    const int64_t seq = kMaxLen;
    const int numLabels = 7;

    std::vector<int64_t> ids(tok.ids, tok.ids + kMaxLen);
    std::vector<int64_t> mask(tok.mask, tok.mask + kMaxLen);

    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    std::vector<int64_t> shape{1, seq};
    Ort::Value idsTensor = Ort::Value::CreateTensor<int64_t>(mem, ids.data(), ids.size(), shape.data(), 2);
    Ort::Value maskTensor = Ort::Value::CreateTensor<int64_t>(mem, mask.data(), mask.size(), shape.data(), 2);

    std::vector<const char*> inNames;
    std::vector<Ort::Value> inVals;
    for (const auto& n : impl_->inputNames) {
        if (n == "input_ids") { inNames.push_back(n.c_str()); inVals.push_back(std::move(idsTensor)); }
        else if (n == "attention_mask") { inNames.push_back(n.c_str()); inVals.push_back(std::move(maskTensor)); }
    }

    // 显式请求 emissions 输出（新版 ORT 不允许 0 输出）
    const char* outNames[] = {impl_->emissionsName.c_str()};
    auto outputs = impl_->session->Run(Ort::RunOptions{nullptr}, inNames.data(), inVals.data(),
                                       inVals.size(), outNames, 1);
    // emissions: [1, MAX_LEN, numLabels]，平铺索引 = t*numLabels + j
    if (impl_->emissionsIndex >= outputs.size()) throw std::runtime_error("RaNER: no emissions output");
    const Ort::Value& emRef = outputs[impl_->emissionsIndex];
    if (!emRef.IsTensor()) throw std::runtime_error("RaNER: emissions not a tensor");

    auto typeInfo = emRef.GetTensorTypeAndShapeInfo();
    size_t flatLen = typeInfo.GetElementCount();
    const float* emissions = emRef.GetTensorData<float>();
    (void)flatLen;

    std::vector<int> pred =
        Viterbi(emissions, tok.mask, kMaxLen, _transitions.data(), _startTransitions.data(),
                _endTransitions.data(), numLabels);

    // 把 word 级别标签折叠回字符（去重连续相同 word idx 的 padding）；
    // padding 位（超出 seq_end）标签为 O(0)，与 test_int8.py 的 `pred_tags[i] if i < len else 0` 一致
    auto cps = Utf8ToCodePoints(text);
    std::vector<std::pair<std::string, std::string>> result;
    int prevWordIdx = -1;
    for (int i = 0; i < kMaxLen; ++i) {
        int widx = tok.wIdx[i];
        if (widx < 0 || widx == prevWordIdx) continue;
        result.emplace_back(CodePointsToUtf8({cps[widx]}), kLabels[pred[i]]);
        prevWordIdx = widx;
    }
    return result;
}

}  // namespace vta

// jni/asr/recognizer.h —— 翻译自 Asr/SherpaAsrBridge.cs 的"上游"部分：
// 不再跨进程连 sherpa-onnx-online-websocket-server，而是进程内直调 sherpa-onnx C++ 库
// 起 OnlineRecognizer / OnlineStream（方案 §0.2/§5.1）。
#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace vta {

// 单会话流式识别循环的回调语义 = 现网 WS 下行 JSON {text, is_final} 的 C++ 化：
//   partial: 识别中（is_final=false）；final: 端点切句/收尾（is_final=true）。
using AsrEventFn = std::function<void(const std::string& text, bool isFinal)>;

// OnlineRecognizer 包装（进程内单例，热词文件变更时整体重建）。
class AsrRecognizer {
public:
    struct Config {
        std::string modelDir;          // encoder/decoder/joiner/tokens 所在目录
        std::string encoder;            // 文件名（相对 modelDir）
        std::string decoder;
        std::string joiner;
        std::string tokens;
        std::string decodingMethod = "modified_beam_search";
        int numThreads = 4;
        // 建模单元：与模型 tokens.txt 一致
        //   "cjkchar"（默认，安全）= 逐字查表
        //   "bbpe"                 = 字节级 BPE；**必须同时提供 bpeVocab**，
        //                            否则 sherpa EncodeHotwords 会空指针崩溃
        std::string modelingUnit = "cjkchar";
        std::string bpeVocab;  // BBPE 编码器（bpe.model）；modelingUnit=bbpe 时必填
        // 端点：三条规则按 utterance 序号轮换，全部设同一停顿阈值（与现网一致，默认 2.0s）
        bool enableEndpoint = true;
        double rule1TrailingSilence = 2.0;
        double rule2TrailingSilence = 2.0;
        double rule3TrailingSilence = 2.0;
        // 热词（解码 bias，字级 token 空格分隔）
        std::string hotwordsFile;
        double hotwordsScore = 2.0;
        // sherpa 内置 HomophoneReplacer（默认空——同音纠正由本进程 HR 模块做，与 C# 管线一致）
        std::string hrLexicon;
        std::string hrRuleFsts;
        std::string hrRuleFars;
    };

    explicit AsrRecognizer(const Config& config);
    ~AsrRecognizer();
    AsrRecognizer(const AsrRecognizer&) = delete;
    AsrRecognizer& operator=(const AsrRecognizer&) = delete;

    // 每会话一条流（模型共享，内存 ~1.2GB 只花在 recognizer 上）
    class Stream {
    public:
        Stream(const AsrRecognizer* owner, void* impl);
        ~Stream();
        Stream(const Stream&) = delete;
        Stream& operator=(const Stream&) = delete;

        // 16kHz float32 PCM 入流
        void AcceptWaveform(const float* samples, int n);
        // 收尾：InputFinished + 排空解码
        void InputFinished();

    private:
        friend class AsrRecognizer;
        const AsrRecognizer* owner_;
        void* impl_;  // StreamHolder*（定义见 .cc 前部）
    };

    std::unique_ptr<Stream> CreateStream() const;
    // 按流传热词（sherpa CreateStream(hotwords)）：多短语用 "/" 分隔。
    // 识别器本体常驻不重建——不同表/不同会话各带各的热词，互不影响。
    std::unique_ptr<Stream> CreateStream(const std::string& hotwords) const;

    // 解码泵：能解就解（等价 WS 服务端内部循环）。返回本次解码后的当前文本（可能为空）。
    std::string PumpDecode(Stream* stream) const;

    // 端点判定（enableEndpoint=false 时恒 false）
    bool IsEndpoint(Stream* stream) const;
    // 端点触发后取最终文本并复位流（等价 sherpa server 切句行为）
    std::string TakeEndpointResult(Stream* stream) const;
    // 复位流（清空解码状态，保留模型已预热的内核/内存池）
    void ResetStream(Stream* stream) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    Config config_;
};

// 16kHz float32 PCM 入队管道（pushPcm / AAudio 回调 → 识别线程）。
// 有界：超上限丢弃最旧数据（识别积压保护），与"实时性优先"的现网语义一致。
class PcmPipe {
public:
    explicit PcmPipe(size_t maxSamples = 16000 * 10 /*10s*/);

    void Push(const float* samples, size_t n);
    // 阻塞取一块（最多 maxN 样本；等待至多 timeoutMs）；返回取到的样本数（0=超时且关闭）
    size_t Pop(float* dst, size_t maxN, int timeoutMs);
    void Close();
    bool Closed() const;
    size_t Buffered() const;

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::vector<float> buf_;
    size_t maxSamples_;
    bool closed_ = false;
};

}  // namespace vta

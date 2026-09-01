namespace VoiceTableAssist.Asr;

/// <summary>sherpa-onnx 服务进程配置。</summary>
internal sealed record SherpaServerOptions(
    string ExePath,
    string Encoder,
    string Decoder,
    string Joiner,
    string Tokens,
    string DecodingMethod,
    string? HotwordsFile,
    double HotwordsScore,
    int Port,
    int NumThreads,
    int StartupTimeoutSeconds,
    bool EnableEndpoint,
    double Rule1TrailingSilence,
    double Rule2TrailingSilence,
    double Rule3TrailingSilence)
{
    public static SherpaServerOptions From(IConfiguration configuration)
    {
        var section = configuration.GetSection("SherpaServer");
        return new SherpaServerOptions(
            section["ExePath"] ?? "sherpa-onnx/sherpa-onnx-online-websocket-server.exe",
            section["Encoder"] ?? "sherpa-onnx/models/sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30/encoder.int8.onnx",
            section["Decoder"] ?? "sherpa-onnx/models/sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30/decoder.onnx",
            section["Joiner"] ?? "sherpa-onnx/models/sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30/joiner.int8.onnx",
            section["Tokens"] ?? "sherpa-onnx/models/sherpa-onnx-streaming-zipformer-zh-int8-2025-06-30/tokens.txt",
            section["DecodingMethod"] ?? "modified_beam_search",
            section["HotwordsFile"],
            section.GetValue("HotwordsScore", 2.0),
            section.GetValue("Port", 6006),
            section.GetValue("NumThreads", 4),
            section.GetValue("StartupTimeoutSeconds", 30),
            // 端点检测：说话停顿后由 sherpa 主动输出 final 并重置识别流——
            // 不开启的话流式识别永远不发 final，服务端"静默自动提交"无从触发。
            section.GetValue("EnableEndpoint", true),
            // sherpa 的三条端点规则按 utterance 序号轮换：第 1 句用 rule1、第 2 句用 rule2、
            // 第 3 句起用 rule3。三条都设成同一停顿阈值，保证每句都能断句并触发提交；
            // 若像旧版把 rule2/3 设成超大值，则只有第一句能断句，后续语音永不提交。
            section.GetValue("Rule1TrailingSilence", 2.0),   // 停顿 2s 切句
            section.GetValue("Rule2TrailingSilence", 2.0),
            section.GetValue("Rule3TrailingSilence", 2.0));
    }
}
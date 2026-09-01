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
            section.GetValue("Rule1TrailingSilence", 2.0),   // 停顿 2s 切句
            section.GetValue("Rule2TrailingSilence", 3600.0), // 禁用默认的 1.2s 激进规则
            section.GetValue("Rule3TrailingSilence", 3600.0)); // 禁用 rule3
    }
}
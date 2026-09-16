namespace VoiceTableAssist.Asr;

/// <summary>
/// 进程内 sherpa-onnx 流式识别配置（原 sherpa-onnx-online-websocket-server 子进程参数一一对应）。
/// 识别器进程内常驻，热词**不再走文件**：按流（<c>CreateStream(hotwords)</c>）随连接传入，
/// 因此没有端口/可执行文件/热词文件等进程级配置。
/// </summary>
internal sealed record SherpaNativeOptions(
    string NativeDir,
    string Encoder,
    string Decoder,
    string Joiner,
    string Tokens,
    string DecodingMethod,
    string ModelingUnit,
    string BpeVocab,
    string Provider,
    int NumThreads,
    int MaxActivePaths,
    double HotwordsScore,
    bool EnableEndpoint,
    double Rule1TrailingSilence,
    double Rule2TrailingSilence,
    double Rule3TrailingSilence,
    string ExpectedNativeVersion,
    int SampleRate)
{
    public static SherpaNativeOptions From(IConfiguration configuration)
    {
        var s = configuration.GetSection("SherpaServer");
        return new SherpaNativeOptions(
            s["NativeDir"] ?? "models/sherpa-onnx",
            s["Encoder"] ?? "models/asr/sherpa-onnx-streaming-zipformer-zh-2025-06-30/encoder.onnx",
            s["Decoder"] ?? "models/asr/sherpa-onnx-streaming-zipformer-zh-2025-06-30/decoder.onnx",
            s["Joiner"] ?? "models/asr/sherpa-onnx-streaming-zipformer-zh-2025-06-30/joiner.onnx",
            s["Tokens"] ?? "models/asr/sherpa-onnx-streaming-zipformer-zh-2025-06-30/tokens.txt",
            s["DecodingMethod"] ?? "modified_beam_search",
            // 建模单元：与 tokens.txt 一致（对齐安卓 vta_config 的 asrModelingUnit 默认值）。
            // ⚠️ 本仓库模型（multi-zh-hans zipformer，BBPE 2000 词表）**发布包不含 bpe.model**：
            //    配 "bbpe" 而 bpe_vocab 为空时 sherpa 的 EncodeHotwords 会空指针崩溃；
            //    故默认 "cjkchar"（逐字查表，安全，热词仅对 tokens.txt 中存在的字生效）。
            //    若拿到配套 bpe.model，设 "bbpe" + BpeVocab=<bpe.model 路径> 即可让热词全量生效。
            s["ModelingUnit"] ?? "cjkchar",
            s["BpeVocab"] ?? "",
            s["Provider"] ?? "cpu",
            s.GetValue("NumThreads", 4),
            s.GetValue("MaxActivePaths", 4),
            s.GetValue("HotwordsScore", 2.0),
            // 端点检测：说话停顿后 sherpa 输出 final 并重置识别流——不开启则流式识别永不发 final，
            // 服务端"静默自动提交"无从触发。
            s.GetValue("EnableEndpoint", true),
            // sherpa 三条端点规则按 utterance 序号轮换：三条同阈值 → 每句停顿即切句输出 final。
            s.GetValue("Rule1TrailingSilence", 2.0),
            s.GetValue("Rule2TrailingSilence", 2.0),
            s.GetValue("Rule3TrailingSilence", 2.0),
            // 绑定（Asr/SherpaNative.cs）对齐的原生库版本：不一致时告警，避免旧 models/ 里的
            // 老 sherpa 原生库与新结构体定义混用（可能加载失败或崩溃）。
            s["ExpectedNativeVersion"] ?? "1.13.6",
            s.GetValue("SampleRate", 16000));
    }

    /// <summary>原生库目录（c-api.dll / onnxruntime.dll 所在）：按 exe 目录相对解析，不存在则回退模型根。</summary>
    public string ResolveNativeDir()
    {
        var rel = StripModelsPrefix(NativeDir);
        var candidates = new[]
        {
            Path.IsPathRooted(NativeDir) ? NativeDir : Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, NativeDir)),
            Path.GetFullPath(Path.Combine(GetModelRoot(), rel)),
        };
        foreach (var dir in candidates)
            if (Directory.Exists(dir)) return dir;
        return candidates[0];
    }

    /// <summary>模型/原生库资产：先按 exe 目录相对解析，不存在则回退模型根（RANER_MODEL_DIR / exe/models）。</summary>
    public static string ResolveAsset(string rel)
    {
        var p = Path.IsPathRooted(rel) ? rel : Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, rel));
        if (File.Exists(p)) return p;

        var fallback = Path.GetFullPath(Path.Combine(GetModelRoot(), StripModelsPrefix(rel)));
        return File.Exists(fallback) ? fallback : p;
    }

    private static string StripModelsPrefix(string rel) =>
        rel.StartsWith("models/", StringComparison.OrdinalIgnoreCase) ? rel["models/".Length..] : rel;

    /// <summary>模型根目录：RANER_MODEL_DIR 优先，否则 exe 目录下的 models/（与 ModelBootstrap 一致）。</summary>
    public static string GetModelRoot()
    {
        var envModels = Environment.GetEnvironmentVariable("RANER_MODEL_DIR");
        return string.IsNullOrWhiteSpace(envModels)
            ? Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "models"))
            : Path.GetFullPath(envModels);
    }
}

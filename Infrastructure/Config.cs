namespace VoiceTableAssist.Infrastructure;

// --------------------------- 配置模型 ---------------------------

/// <summary>本地 sherpa-onnx 流式识别服务配置。</summary>
internal sealed record SherpaOptions(string Endpoint, int SampleRate)
{
    public static SherpaOptions From(IConfiguration configuration)
    {
        var section = configuration.GetSection("AsrProvider");
        return new SherpaOptions(
            section["Endpoint"] ?? "ws://127.0.0.1:6006",
            section.GetValue("SampleRate", 16000));
    }
}

/// <summary>
/// 进程内 PCM 降噪配置（GTCRN）。仅作用于网关 -> sherpa-onnx 这一段上行音频。
/// 默认关闭——开启前需要 C# 侧 DSP（STFT/ISTFT + 状态缓存）已对照 sherpa-onnx 参考实现
/// 完成数值一致性自检，避免错误的相位恢复导致端点判定/ASR 精度恶化。
/// 完整配置示例见 appsettings.json 与 [相关文档/部署文档.md] "GTCRN 降噪" 一节。
/// </summary>
internal sealed record DenoiseOptions(bool Enabled, string ModelPath, int NumThreads, int SampleRate)
{
    public static DenoiseOptions From(IConfiguration configuration)
    {
        var section = configuration.GetSection("AsrProvider:Denoise");
        var modelPath = section["ModelPath"] ?? "models/asr/gtcrn_simple.onnx";
        // 开发环境模型在源码 models/（RANER_MODEL_DIR 覆盖）；相对路径按 exe 目录解析。
        var envModels = Environment.GetEnvironmentVariable("RANER_MODEL_DIR");
        if (!string.IsNullOrWhiteSpace(envModels) && !Path.IsPathRooted(modelPath))
            modelPath = Path.GetFullPath(Path.Combine(envModels, modelPath.Replace("models/", "", StringComparison.OrdinalIgnoreCase)));
        else if (!Path.IsPathRooted(modelPath))
            modelPath = Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, modelPath));
        return new DenoiseOptions(
            section.GetValue("Enabled", false),
            modelPath,
            section.GetValue("NumThreads", 1),
            section.GetValue("SampleRate", 16000));
    }
}
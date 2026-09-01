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
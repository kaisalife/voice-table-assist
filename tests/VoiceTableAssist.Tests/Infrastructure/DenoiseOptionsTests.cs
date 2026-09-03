using FluentAssertions;
using Microsoft.Extensions.Configuration;
using VoiceTableAssist.Infrastructure;
using Xunit;

namespace VoiceTableAssist.Tests.Infrastructure;

/// <summary>
/// GTCRN 降噪配置加载测试：appsettings.json → DenoiseOptions 的绑定正确性。
/// DSP 实现相关的行为不在此覆盖（passthrough 由 GtcrnDenoiser.cs 注释明确声明）。
/// </summary>
public class DenoiseOptionsTests
{
    [Fact]
    public void From_Defaults_WhenSectionMissing()
    {
        var cfg = new ConfigurationBuilder()
            .AddInMemoryCollection(new Dictionary<string, string?>
            {
                ["Other:Key"] = "value"
            })
            .Build();

        var opts = DenoiseOptions.From(cfg);

        opts.Enabled.Should().BeFalse();
        // From() 把相对路径解析为绝对路径（无 RANER_MODEL_DIR 时相对 exe 目录）
        opts.ModelPath.Should().NotBeNullOrWhiteSpace().And.EndWith("gtcrn_simple.onnx");
        Path.IsPathRooted(opts.ModelPath).Should().BeTrue();
        opts.NumThreads.Should().Be(1);
        opts.SampleRate.Should().Be(16000);
    }

    [Fact]
    public void From_ReadsEnabledTrue()
    {
        var cfg = new ConfigurationBuilder()
            .AddInMemoryCollection(new Dictionary<string, string?>
            {
                ["AsrProvider:Denoise:Enabled"] = "true",
                ["AsrProvider:Denoise:ModelPath"] = "custom/path/denoiser.onnx",
                ["AsrProvider:Denoise:NumThreads"] = "2",
                ["AsrProvider:Denoise:SampleRate"] = "16000"
            })
            .Build();

        var opts = DenoiseOptions.From(cfg);

        opts.Enabled.Should().BeTrue();
        opts.ModelPath.Should().EndWith("denoiser.onnx");
        Path.IsPathRooted(opts.ModelPath).Should().BeTrue();
        opts.NumThreads.Should().Be(2);
        opts.SampleRate.Should().Be(16000);
    }

    [Fact]
    public void From_ReadsEnabledFalse()
    {
        var cfg = new ConfigurationBuilder()
            .AddInMemoryCollection(new Dictionary<string, string?>
            {
                ["AsrProvider:Denoise:Enabled"] = "false"
            })
            .Build();

        var opts = DenoiseOptions.From(cfg);

        opts.Enabled.Should().BeFalse();
    }
}

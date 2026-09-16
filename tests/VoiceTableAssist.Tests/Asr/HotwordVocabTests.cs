using FluentAssertions;
using VoiceTableAssist.Asr;
using Xunit;

namespace VoiceTableAssist.Tests.Asr;

/// <summary>
/// 热词可用性过滤：短语必须能**完整**编码才加载为热词。
/// 依据 sherpa-onnx v1.13.6 utils.cc:EncodeBase —— 查不到的字只是打日志，该短语仍会被塞进解码图但只剩查得到的字
/// （「外 径」→ 单字「外」，「光 洁 度」→ 伪词「光度」），属于错误偏置；故整条不加载，交由读音吸附兜底。
/// </summary>
public class HotwordVocabTests
{
    private static string WriteTokens(string content)
    {
        var path = Path.Combine(Path.GetTempPath(), $"vta-tokens-{Guid.NewGuid():N}.txt");
        File.WriteAllText(path, content);
        return path;
    }

    [Fact]
    public void OnlySingleCharTokens_AreUsable()
    {
        // 含字节 token(<0xNN>)、多字片段(▁第)、单字 token
        var path = WriteTokens("<blk> 0\n<sos/eos> 1\n<unk> 2\n<0xE5> 3\n▁第 4\n硬 5\n度 6\n外 7\n光 8\n");
        try
        {
            var vocab = HotwordVocab.Get(path);
            vocab.Should().NotBeNull();
            vocab!.Count.Should().Be(4);                 // 硬/度/外/光（字节 token 与多字片段都不算）
            vocab.CanEncode("硬度").Should().BeTrue();
            vocab.CanEncode("外径").Should().BeFalse();    // 径 缺 → 整条不可编码
            vocab.CanEncode("光洁度").Should().BeFalse();  // 洁 缺
            vocab.CanEncode("").Should().BeFalse();
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void MissingTokensFile_ReturnsNull_MeaningNoFiltering()
    {
        HotwordVocab.Get(Path.Combine(Path.GetTempPath(), $"vta-nope-{Guid.NewGuid():N}.txt")).Should().BeNull();
    }
}

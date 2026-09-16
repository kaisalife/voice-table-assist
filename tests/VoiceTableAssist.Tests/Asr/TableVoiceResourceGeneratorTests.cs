using FluentAssertions;
using VoiceTableAssist.Asr;
using Xunit;

namespace VoiceTableAssist.Tests.Asr;

/// <summary>
/// 按表生成热词（行标签 + 列描述符 + 单字数字）与**按流传词串**格式：
/// sherpa 解码 bias 用字级 token（每字空格分隔），按流传入时多短语用 '/' 连接。
/// </summary>
public class TableVoiceResourceGeneratorTests
{
    [Fact]
    public void BuildHotWords_SpaceSeparatesChars_AndSkipsNonHanPhrases()
    {
        var text = TableVoiceResourceGenerator.BuildHotWords(["水位"], 1);
        var lines = text.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);

        lines.Should().Contain("水 位");
        lines.Should().Contain("一 号");
        lines.Should().Contain("测 量 值 一");
        // 含 ASCII 数字的写法模型输出不了，必须跳过（1号/第1列…）
        lines.Should().NotContain(l => l.Contains('1'));
        lines.Should().NotContain("1 号");
        lines.Should().NotContain("第 1 列");
    }

    [Fact]
    public void BuildHotWords_ExcludesTen_ButKeepsPointAndDigits()
    {
        var lines = TableVoiceResourceGenerator.BuildHotWords(["水位"], 1)
            .Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);

        lines.Should().Contain(["零", "一", "二", "三", "四", "五", "六", "七", "八", "九", "点"]);
        lines.Should().NotContain("十");   // 「十」与「点」抢分，会把"一点二"压成"十二"
    }

    [Fact]
    public void BuildHotWordsStream_JoinsPhrasesWithSlash()
    {
        var stream = TableVoiceResourceGenerator.BuildHotWordsStream(["水位"], 1);
        var parts = stream.Split('/');

        parts.Should().Contain(["水 位", "一 号", "测 量 值 一", "点"]);
        parts.Should().NotContain(p => p.Length == 0);
        stream.Should().NotContain("\n");
        stream.Should().StartWith("水 位/一 号");
    }

    [Fact]
    public void BuildHotWordsStream_ColumnCountBeyondTen_UsesHanDigits()
    {
        var stream = TableVoiceResourceGenerator.BuildHotWordsStream(["水位"], 12);
        stream.Split('/').Should().Contain("十 二 号");
    }

    [Fact]
    public void ToChineseNum_MatchesColumnDescriptors()
    {
        TableVoiceResourceGenerator.ToChineseNum(6).Should().Be("六");
        TableVoiceResourceGenerator.ToChineseNum(10).Should().Be("十");
        TableVoiceResourceGenerator.ToChineseNum(11).Should().Be("十一");
        TableVoiceResourceGenerator.ToChineseNum(24).Should().Be("二十四");
    }

    [Fact]
    public void PhraseWithoutFullEncoding_IsSkippedEntirely_NotTruncated()
    {
        // 模拟真实模型：有 硬/度/号/一…，没有 径/洁
        var tokensPath = Path.Combine(Path.GetTempPath(), $"vta-tokens-{Guid.NewGuid():N}.txt");
        File.WriteAllText(tokensPath, "硬 0\n度 1\n号 2\n一 3\n外 4\n光 5\n测 6\n量 7\n值 8\n序 9\n第 10\n个 11\n列 12\n零 13\n二 14\n三 15\n四 16\n五 17\n六 18\n七 19\n八 20\n九 21\n点 22\n");
        try
        {
            var vocab = HotwordVocab.Get(tokensPath);
            var (text, skipped) = TableVoiceResourceGenerator.BuildHotWordsReport(["外径", "内径", "光洁度", "硬度"], 1, vocab);

            skipped.Should().BeEquivalentTo(["外径", "内径", "光洁度"]);
            text.Should().Contain("硬 度");
            text.Should().Contain("一 号");
            // 绝不能出现截断产物（单字「外」/伪词「光 度」）
            text.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries)
                .Should().NotContain(["外", "光", "光 度", "内"]);
        }
        finally { File.Delete(tokensPath); }
    }

    [Fact]
    public void StreamReport_ReportsSkippedPhrases()
    {
        var tokensPath = Path.Combine(Path.GetTempPath(), $"vta-tokens-{Guid.NewGuid():N}.txt");
        File.WriteAllText(tokensPath, "硬 0\n度 1\n号 2\n一 3\n");
        try
        {
            var (stream, skipped) = TableVoiceResourceGenerator.BuildHotWordsStreamReport(["硬度", "外径"], 1, HotwordVocab.Get(tokensPath));
            skipped.Should().Contain("外径");
            stream.Should().Contain("硬 度");
            stream.Should().NotContain("外");
        }
        finally { File.Delete(tokensPath); }
    }

    [Fact]
    public void WithoutVocab_NoFiltering_LegacyBehaviour()
    {
        var text = TableVoiceResourceGenerator.BuildHotWords(["外径", "硬度"], 0, null);
        text.Should().Contain("外 径");
        text.Should().Contain("硬 度");
    }
}

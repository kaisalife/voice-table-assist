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
}

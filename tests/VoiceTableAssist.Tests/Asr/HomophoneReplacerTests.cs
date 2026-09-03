using FluentAssertions;
using VoiceTableAssist.Asr;
using Xunit;

namespace VoiceTableAssist.Tests.Asr;

/// <summary>
/// 同音字/专名替换：汉字→拼音→整串拼音命中→输出正确汉字。
/// 真实场景：ASR 把表里行标签误识为近音错字。
/// 测例用"甲乙丙丁/ABCD"做抽象字面值，不依赖具体汉字读音，便于在非中文环境复现。
/// </summary>
public class HomophoneReplacerTests
{
    private static HomophoneReplacer Build(string pinyin, string[] rules)
    {
        var path = Path.GetTempFileName();
        File.WriteAllText(path, pinyin);
        try
        {
            return new HomophoneReplacer(path, rules);
        }
        finally
        {
            File.Delete(path);
        }
    }

    // 拼音表：用甲乙丙丁做"待替换字"（错误字），ABCD 是它们的"正确字"。
    // 共享同一组拼音前缀 y*：甲=ya 乙=yb 丙=yc 丁=yd / A=a B=b C=c D=d
    // "甲"和"A"同属 y* 前缀族但长度不同（ya vs a），ASR 误识为甲 → 应通过规则 ya→A 还原。
    // 长度必须严格递增（甲乙→yayb 长于 ya），这样 longest-match 逻辑能正确选出 yayb=AB。
    private const string Pingyin = "甲=ya\n乙=yb\n丙=yc\n丁=yd\nA=a\nB=b\nC=c\nD=d\n";

    [Fact]
    public void NoRules_PassesThrough()
    {
        var r = Build(Pingyin, Array.Empty<string>());
        r.Enabled.Should().BeTrue();   // 拼音表存在 → 启用
        r.Apply("甲乙").Should().Be("甲乙");
    }

    [Fact]
    public void EnabledFalse_WhenPinyinEmpty()
    {
        var path = Path.GetTempFileName();
        File.WriteAllText(path, "");
        try
        {
            var r = new HomophoneReplacer(path, new[] { "ya=A" });
            r.Enabled.Should().BeFalse();
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Fact]
    public void SingleRule_Replaces()
    {
        var r = Build(Pingyin, new[] { "ya=A" });
        r.Apply("甲").Should().Be("A");
    }

    [Fact]
    public void DifferentChar_SamePinyin_Replaces()
    {
        // 测核心纠错：ASR 把表里"AB"误识为"甲乙"（拼音都是 ya+yb），
        // 规则 yayb=AB 命中 → 还原成正确的表标签。
        var r = Build(Pingyin, new[] { "yayb=AB" });
        r.Apply("甲乙").Should().Be("AB");
    }

    [Fact]
    public void LongestMatchWins()
    {
        // 两条规则 ya=A 和 yayb=AB → "甲乙" 选最长
        var r = Build(Pingyin, new[] { "ya=A", "yayb=AB" });
        r.Apply("甲乙").Should().Be("AB");
    }

    [Fact]
    public void NonHanPassThrough()
    {
        var r = Build(Pingyin, new[] { "yayb=AB" });
        r.Apply("甲乙 12.5 号").Should().Be("AB 12.5 号");
    }

    [Fact]
    public void MixedPartial()
    {
        // 多个独立片段拼起来；每个"甲乙"独立命中规则
        var r = Build(Pingyin, new[] { "yayb=AB" });
        r.Apply("甲乙 丙丁甲乙").Should().Be("AB 丙丁AB");
    }

    [Fact]
    public void ToTone3Pinyin_FromTable()
    {
        var r = Build(Pingyin, Array.Empty<string>());
        r.ToTone3Pinyin("甲乙").Should().Be("yayb");
    }

    [Fact]
    public void ToTone3Pinyin_UnknownChar_PassesThrough()
    {
        var r = Build(Pingyin, Array.Empty<string>());
        // 选一个肯定不在表里的字符（极生僻字）
        r.ToTone3Pinyin("䶮").Should().Be("䶮");
    }

    [Fact]
    public void EmptyText_PassesThrough()
    {
        var r = Build(Pingyin, new[] { "yayb=AB" });
        r.Apply("").Should().Be("");
    }

    [Fact]
    public void RuleSkipsCommentsAndBlankLines()
    {
        // 注释行（# 开头）和空行应该被忽略
        var r = Build(Pingyin, new[] { "# 这是注释", "", "yayb=AB" });
        r.Apply("甲乙").Should().Be("AB");
    }

    [Fact]
    public void SpaceSeparatedPinyinFormat_AlsoSupported()
    {
        // 拼音表也支持"汉字 pinyin"空格分隔的格式
        var path = Path.GetTempFileName();
        File.WriteAllText(path, "甲 ya\n乙 yb\n");
        try
        {
            var r = new HomophoneReplacer(path, new[] { "yayb=AB" });
            r.Apply("甲乙").Should().Be("AB");
        }
        finally
        {
            File.Delete(path);
        }
    }
}

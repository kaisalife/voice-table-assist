using FluentAssertions;
using VoiceTableAssist.Asr;
using Xunit;

namespace VoiceTableAssist.Tests.Asr;

/// <summary>
/// 通用数字同音归一（与表无关）：只纠正**声调相同**的真同音，且必须处于数字上下文，
/// 避免"实在/时间"这类词被误改（对齐 Android jni/homophone/domain_correction.cc）。
/// 表相关的同音错字（印度→硬度、二好→二号…）已由表内读音吸附覆盖，不在此层。
/// </summary>
public class DomainCorrectionTests
{
    [Theory]
    [InlineData("五实", "五十")]      // 实 shí → 十
    [InlineData("五石", "五十")]
    [InlineData("五时", "五十")]
    [InlineData("灵点零", "零点零")]   // 灵 líng → 零
    [InlineData("付五点零", "负五点零")] // 付 fù → 负
    [InlineData("四寺", "四四")]      // 寺 sì → 四
    [InlineData("五巴", "五八")]      // 巴 bā → 八
    [InlineData("九久", "九九")]      // 久 jiǔ → 九
    [InlineData("五午", "五五")]      // 午 wǔ → 五
    [InlineData("五武", "五五")]      // 武 wǔ → 五
    [InlineData("迁千", "千千")]      // 迁 qiān → 千
    public void NumeralHomophone_InNumericContext_IsNormalized(string input, string expected)
        => DomainCorrection.Correct(input).Should().Be(expected);

    [Theory]
    [InlineData("实在")]
    [InlineData("时间")]
    [InlineData("工作时间")]
    [InlineData("付出")]
    [InlineData("一号是五十")]
    [InlineData("水位二号六十点零")]
    public void NonNumericContext_IsUntouched(string text)
        => DomainCorrection.Correct(text).Should().Be(text);

    [Fact]
    public void Correction_IsIdempotent()
    {
        var once = DomainCorrection.Correct("五实点灵付三");
        DomainCorrection.Correct(once).Should().Be(once);
    }

    [Fact]
    public void EmptyOrWhitespace_PassesThrough()
    {
        DomainCorrection.Correct("").Should().Be("");
        DomainCorrection.Correct("   ").Should().Be("   ");
    }
}

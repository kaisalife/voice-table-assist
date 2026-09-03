using FluentAssertions;
using VoiceTableAssist.Services;
using Xunit;

namespace VoiceTableAssist.Tests.Services;

/// <summary>
/// 中文数字(零一二…十百千点) -> 阿拉伯数字，范围 0~1000，保留两位小数。
/// 真实场景：硬编码/口语化 ASR 输出的中文数值片段。
/// </summary>
public class ChineseNumeralTests
{
    [Fact]
    public void Zero_纯零()
    {
        ChineseNumeral.ToDecimal("零").Should().Be(0);
    }

    [Fact]
    public void Zero_圆圈字符()
    {
        // 同义词："〇" 在中文数字里也表示 0
        ChineseNumeral.ToDecimal("〇").Should().Be(0);
    }

    [Fact]
    public void SingleDigit()
    {
        ChineseNumeral.ToDecimal("五").Should().Be(5);
    }

    [Fact]
    public void TwoDigits_LianXie()
    {
        ChineseNumeral.ToDecimal("十一").Should().Be(11);
    }

    [Fact]
    public void TwoDigits_DanWei()
    {
        // "二十" = 20
        ChineseNumeral.ToDecimal("二十").Should().Be(20);
    }

    [Fact]
    public void ThreeDigits()
    {
        // "九十五" = 95
        ChineseNumeral.ToDecimal("九十五").Should().Be(95);
    }

    [Fact]
    public void FourDigits_Qian()
    {
        // "一千" = 1000（项目上界）
        ChineseNumeral.ToDecimal("一千").Should().Be(1000);
    }

    [Fact]
    public void Bai()
    {
        // "一百二十三" = 123
        ChineseNumeral.ToDecimal("一百二十三").Should().Be(123);
    }

    [Fact]
    public void WithDecimal_1Digit()
    {
        // "十一点五" = 11.5
        ChineseNumeral.ToDecimal("十一点五").Should().Be(11.5);
    }

    [Fact]
    public void WithDecimal_2Digits()
    {
        // "零点零五" = 0.05
        ChineseNumeral.ToDecimal("零点零五").Should().Be(0.05);
    }

    [Fact]
    public void WithDecimal_DianHao()
    {
        // "。" 在中文文本里也常被 ASR 误识别为小数点
        ChineseNumeral.ToDecimal("一。二").Should().Be(1.2);
    }

    [Fact]
    public void Negative_负号()
    {
        // "负九十五" = -95（巡检负压/真空场景）
        ChineseNumeral.ToDecimal("负九十五").Should().Be(-95);
    }

    [Fact]
    public void NegativeWithDecimal()
    {
        ChineseNumeral.ToDecimal("负五点零").Should().Be(-5.0);
    }

    [Fact]
    public void EmbedInLargerText_PicksFirstMatch()
    {
        // 真实场景：NER 抽出的 val 字段可能带前后杂质（"十一 点 五"），ToDecimal
        // 贪婪取最长中文数字片段，再做累加解析。期望 11.5。
        ChineseNumeral.ToDecimal("十一点五").Should().Be(11.5);
    }

    [Fact]
    public void OutOfRange_ReturnsZero()
    {
        // "一千零一" 超出 [0, 1000] 范围 → 返回 0
        ChineseNumeral.ToDecimal("一千零一").Should().Be(0);
    }

    [Fact]
    public void EmptyString_ReturnsZero()
    {
        ChineseNumeral.ToDecimal("").Should().Be(0);
    }

    [Fact]
    public void NoChineseDigits_ReturnsZero()
    {
        ChineseNumeral.ToDecimal("hello world").Should().Be(0);
    }

    [Fact]
    public void RoundToTwoDecimals()
    {
        // "零点一一一" = 0.111 → 截到 0.11
        ChineseNumeral.ToDecimal("零点一一一").Should().Be(0.11);
    }
}

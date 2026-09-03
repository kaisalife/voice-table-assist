using FluentAssertions;
using VoiceTableAssist.Asr;
using Xunit;

namespace VoiceTableAssist.Tests.Asr;

/// <summary>
/// 流式文本合并：与验证页逻辑一致的前缀扩展 / 包含去重 / 重叠拼接。
/// 是累计累计精简、OnFinal、NER 重算之间的关键衔接点。
/// </summary>
public class MergeTextTests
{
    [Fact]
    public void EmptyPrev_ReturnsCur()
    {
        VoiceInteractionSession.MergeText("", "硬度一号").Should().Be("硬度一号");
    }

    [Fact]
    public void EmptyCur_ReturnsPrev()
    {
        VoiceInteractionSession.MergeText("硬度一号", "").Should().Be("硬度一号");
    }

    [Fact]
    public void BothEmpty_ReturnsEmpty()
    {
        VoiceInteractionSession.MergeText("", "").Should().Be("");
    }

    [Fact]
    public void CurContainedInPrev_ReturnsPrev()
    {
        // partial 是 prev 的子串 → 保留 prev（避免覆盖已 stable 的 final）
        VoiceInteractionSession.MergeText("硬度一号十一点五", "一号十一点五")
            .Should().Be("硬度一号十一点五");
    }

    [Fact]
    public void PrevContainedInCur_ReturnsCur()
    {
        // final 比 prev 长且包含 prev → 信任 final
        VoiceInteractionSession.MergeText("硬度一号", "硬度一号十一点五二号十二点五")
            .Should().Be("硬度一号十一点五二号十二点五");
    }

    [Fact]
    public void Overlap_OneChar_Stitches()
    {
        // 上一段尾巴是下一段开头（重叠 ≥2）→ 去掉重叠
        VoiceInteractionSession.MergeText("硬度一号十", "号十一点五")
            .Should().Be("硬度一号十一点五");
    }

    [Fact]
    public void Overlap_MultiChar_Stitches()
    {
        VoiceInteractionSession.MergeText("硬度一号十", "号十一点五")
            .Should().Be("硬度一号十一点五");
    }

    [Fact]
    public void NoOverlap_JoinsWithSpace()
    {
        // 完全不重叠时退化为"前 + 空格 + 后"，与浏览器侧行为一致
        VoiceInteractionSession.MergeText("硬度一号", "二号十二点五")
            .Should().Be("硬度一号 二号十二点五");
    }

    [Fact]
    public void OverlapBelowThreshold_DoesNotStitch()
    {
        // 重叠小于 2 字符 → 视为无重叠，避免误拼（"一" 单独匹配太多场景）
        VoiceInteractionSession.MergeText("硬度一", "号十一点五")
            .Should().Be("硬度一 号十一点五");
    }

    [Fact]
    public void CurExactlyPrev_NoChange()
    {
        // 完全相同的 final 不会产生重复输出
        VoiceInteractionSession.MergeText("硬度一号", "硬度一号")
            .Should().Be("硬度一号");
    }
}

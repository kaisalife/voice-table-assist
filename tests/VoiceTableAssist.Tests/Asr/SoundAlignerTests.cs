using FluentAssertions;
using VoiceTableAssist.Asr;
using Xunit;

namespace VoiceTableAssist.Tests.Asr;

/// <summary>
/// 表内读音吸附（对齐 Android jni/text/sound_aligner.cc 与《新语音输入同音解决方案.md》）：
/// 忽略声调/前后鼻音/平翘舌/n-l/f-h/音节编辑距离≤1，配合阈值 + 差距 + 精确保护区守卫，
/// 让 ASR 听错的"主体/位置"吸附回本表词表（分不清则放弃，宁缺勿错）。
/// </summary>
public class SoundAlignerTests
{
    // 测试用去声调拼音字典（TONE3，加载时剥声调）
    private const string Dictionary = """
        硬=ying4
        度=du4
        水=shui3
        位=wei4
        卫=wei4
        围=wei2
        印=yin4
        一=yi1
        二=er4
        三=san1
        四=si4
        号=hao4
        好=hao3
        汽=qi4
        压=ya1
        气=qi4
        测=ce4
        量=liang4
        值=zhi2
        序=xu4
        第=di4
        个=ge4
        列=lie4
        十=shi2
        实=shi2
        五=wu3
        六=liu4
        点=dian3
        零=ling2
        负=fu4
        今=jin1
        天=tian1
        不=bu4
        错=cuo4
        """;

    private static readonly PinyinTable Table = LoadTable();

    private static PinyinTable LoadTable()
    {
        var path = Path.Combine(Path.GetTempPath(), "vta-sound-aligner-test-pinyin.txt");
        File.WriteAllText(path, Dictionary);
        return PinyinTable.Get(path)!;
    }

    private static SoundAligner Build(IReadOnlyList<string> rows, int columnCount = 0)
        => new(Table, rows, columnCount);

    // ---- 音近模型 ----

    [Fact]
    public void SyllableSim_SameSyllable_IsOne() => SoundAligner.SyllableSim("ying", "ying").Should().Be(1.0);

    [Theory]
    [InlineData("shui", "sui")]   // 平翘舌
    [InlineData("na", "la")]      // n-l
    [InlineData("fa", "ha")]      // f-h
    public void SyllableSim_InitialClass_IsPointNine(string a, string b)
        => SoundAligner.SyllableSim(a, b).Should().Be(0.9);

    [Theory]
    [InlineData("ying", "yin")]   // 后鼻音/前鼻音
    [InlineData("sang", "san")]
    public void SyllableSim_NasalFinal_IsPointNine(string a, string b)
        => SoundAligner.SyllableSim(a, b).Should().Be(0.9);

    [Fact]
    public void SyllableSim_EditDistanceOne_IsPointSix()
        => SoundAligner.SyllableSim("shi", "shu").Should().Be(0.6);

    [Fact]
    public void SyllableSim_Unrelated_IsZero()
        => SoundAligner.SyllableSim("shui", "hao").Should().Be(0.0);

    [Fact]
    public void SequenceSim_RequiresSameLength()
        => SoundAligner.SequenceSim(["shui", "wei"], ["shui"]).Should().Be(0.0);

    [Fact]
    public void SequenceSim_AnyUnrelatedSyllable_FailsWholeSequence()
        => SoundAligner.SequenceSim(["shui", "wei"], ["shui", "hao"]).Should().Be(0.0);

    // ---- 词表 ----

    [Fact]
    public void NonHanRowLabel_IsSkipped_AndAlignerNotReady()
    {
        var aligner = Build(["PH值"]);
        aligner.Ready.Should().BeFalse();
    }

    [Fact]
    public void ColumnDescriptors_ArePartOfVocabulary()
    {
        var aligner = Build(["水位"], 2);
        // 含 ASCII 数字的写法（1号/第1列…）模型不会输出，不进读音词表；纯汉字写法进词表
        aligner.Vocabulary.Should().Contain(["一号", "二号", "测量值一", "第二列", "序号二", "二号列"]);
        aligner.Vocabulary.Should().NotContain("第1列");
    }

    // ---- 整词吸附 ----

    [Fact]
    public void AlignWord_ExactVocabWord_ReturnsCanonicalWithScoreOne()
    {
        var match = Build(["水位"]).AlignWord("水位");
        match.Canonical.Should().Be("水位");
        match.Score.Should().Be(1.0);
    }

    [Fact]
    public void AlignWord_MisheardRowLabel_Aligned()
    {
        // 水卫（wei）/水位（wei）同音 → 吸附
        var match = Build(["水位"]).AlignWord("水卫");
        match.Canonical.Should().Be("水位");
        match.Score.Should().Be(1.0);
    }

    [Fact]
    public void AlignWord_Unrelated_Rejected()
    {
        var match = Build(["水位"]).AlignWord("今天");
        match.Canonical.Should().BeEmpty();
    }

    // ---- 整句吸附 ----

    [Fact]
    public void AlignSentence_MisheardRowLabel_Aligned()
    {
        var aligner = Build(["水位"], 6);
        aligner.AlignSentence("水卫一号五十").Should().Be("水位一号五十");
    }

    [Fact]
    public void AlignSentence_MisheardColumnDescriptor_Aligned()
    {
        var aligner = Build(["水位"], 6);
        aligner.AlignSentence("水位二好五十").Should().Be("水位二号五十");
    }

    [Fact]
    public void AlignSentence_ExactVocabWords_StayUnchanged()
    {
        var aligner = Build(["硬度", "印度"], 6);
        aligner.AlignSentence("硬度").Should().Be("硬度");   // 与印度近音（ying/yin）也不替换
        aligner.AlignSentence("印度").Should().Be("印度");
    }

    [Fact]
    public void AlignSentence_ValueSegment_Untouched()
    {
        var aligner = Build(["水位"], 6);
        aligner.AlignSentence("水位一号五十点零").Should().Be("水位一号五十点零");
    }

    [Fact]
    public void AlignSentence_UnrelatedSentence_Unchanged()
    {
        var aligner = Build(["水位"], 6);
        aligner.AlignSentence("今天不错").Should().Be("今天不错");
    }

    [Fact]
    public void AlignSentence_HighMarginGuard_RejectsCloseCandidates()
    {
        var aligner = Build(["水位"], 0);
        aligner.Margin = 1.1;   // 要求与次优差距 >1 → 全部放弃
        aligner.AlignSentence("水卫").Should().Be("水卫");
    }

    [Fact]
    public void AlignSentence_ThresholdControlsAcceptance()
    {
        var aligner = Build(["水位"], 0);
        aligner.Threshold = 1.5;   // 阈值不可达 → 放弃
        aligner.AlignSentence("水卫").Should().Be("水卫");
    }

    [Fact]
    public void AlignSentence_WithoutVocabulary_PassesThrough()
    {
        var aligner = Build([], 0);
        aligner.Ready.Should().BeFalse();
        aligner.AlignSentence("水卫一号").Should().Be("水卫一号");
    }

    [Fact]
    public void OnReplace_ReportsReplacement()
    {
        var aligner = Build(["水位"], 6);
        var reported = new List<string>();
        aligner.OnReplace = (original, canonical, _) => reported.Add($"{original}->{canonical}");
        aligner.AlignSentence("水卫一号五十");
        reported.Should().Contain("水卫->水位");
    }
}

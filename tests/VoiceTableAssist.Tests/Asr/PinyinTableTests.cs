using FluentAssertions;
using VoiceTableAssist.Asr;
using Xunit;

namespace VoiceTableAssist.Tests.Asr;

/// <summary>
/// 汉字→去声调拼音字典（表内读音吸附的输入）：TONE3 末尾声调数字被剥掉，空格格式兼容。
/// </summary>
public class PinyinTableTests
{
    private static string WritePinyin(string content)
    {
        var path = Path.Combine(Path.GetTempPath(), $"vta-pinyin-{Guid.NewGuid():N}.txt");
        File.WriteAllText(path, content);
        return path;
    }

    [Fact]
    public void ToneDigit_IsStripped()
    {
        var path = WritePinyin("硬=ying4\n度=du4\n一=yi1\n");
        try
        {
            var table = PinyinTable.Get(path);
            table.Should().NotBeNull();
            table!.Syllable('硬').Should().Be("ying");
            table.Syllable('度').Should().Be("du");
            table.Syllable('一').Should().Be("yi");
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void NeutralToneAndV_AreSupported()
    {
        var path = WritePinyin("的=de5\n绿=lv4\n");
        try
        {
            var table = PinyinTable.Get(path)!;
            table.Syllable('的').Should().Be("de");
            table.Syllable('绿').Should().Be("lv");
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void SpaceSeparatedFormat_AlsoSupported()
    {
        var path = WritePinyin("硬 ying4\n度 du4\n");
        try
        {
            var table = PinyinTable.Get(path)!;
            table.Syllable('硬').Should().Be("ying");
            table.Syllable('度').Should().Be("du");
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void UnknownChar_ReturnsEmpty_AndSyllablesKeepLength()
    {
        var path = WritePinyin("硬=ying4\n");
        try
        {
            var table = PinyinTable.Get(path)!;
            table.Syllable('䶮').Should().Be("");
            var syllables = table.Syllables("硬度");
            syllables.Should().HaveCount(2);
            syllables[0].Should().Be("ying");
            syllables[1].Should().Be("");
        }
        finally { File.Delete(path); }
    }

    [Fact]
    public void MissingFile_ReturnsNull()
    {
        PinyinTable.Get(Path.Combine(Path.GetTempPath(), $"vta-missing-{Guid.NewGuid():N}.txt"))
            .Should().BeNull();
    }

    [Fact]
    public void EmptyFile_ReturnsNull()
    {
        var path = WritePinyin("# 只有注释\n");
        try { PinyinTable.Get(path).Should().BeNull(); }
        finally { File.Delete(path); }
    }
}

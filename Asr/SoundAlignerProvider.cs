namespace VoiceTableAssist.Asr;

/// <summary>
/// 按表构建表内读音吸附器（<see cref="SoundAligner"/>）：
/// 词表 = 该表行标签 + 按列数枚举的位置词，全部自动生成、零人工维护；
/// 拼音字典（hr_char_pinyin.txt）由 <see cref="PinyinTable"/> 进程内缓存，仅加载一次。
/// 该表没有行标签（索引未加载）或拼音字典缺失时返回 null（退化为仅通用数字同音归一）。
/// </summary>
internal static class SoundAlignerProvider
{
    public static SoundAligner? Get(IConfiguration configuration, IReadOnlyList<string>? rows, int columnCount)
    {
        if (rows is null || rows.Count == 0) return null;

        // 拼音字典：先按 exe 目录解析，找不到回退模型根（RANER_MODEL_DIR / exe/models），与 ASR 模型解析一致
        var charPinyin = SherpaNativeOptions.ResolveAsset(
            configuration["Align:CharPinyin"] is { Length: > 0 } p ? p : "sherpa-onnx/hr/hr_char_pinyin.txt");
        var pinyin = PinyinTable.Get(charPinyin);
        if (pinyin is null) return null;

        var aligner = new SoundAligner(pinyin, rows, columnCount)
        {
            Threshold = configuration.GetValue("Align:Threshold", 0.75),
            Margin = configuration.GetValue("Align:Margin", 0.15),
        };
        return aligner.Ready ? aligner : null;
    }
}

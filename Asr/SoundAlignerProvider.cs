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

        // 拼音字典解析顺序：配置路径（默认随代码发布的 assets/hr_char_pinyin.txt，exe 目录优先、
        // 其次模型根）→ 旧部署兼容（models/sherpa-onnx/hr/hr_char_pinyin.txt）。
        var configured = configuration["Align:CharPinyin"] is { Length: > 0 } p ? p : "assets/hr_char_pinyin.txt";
        var pinyin = PinyinTable.Get(SherpaNativeOptions.ResolveAsset(configured));
        if (pinyin is null)
        {
            const string legacy = "sherpa-onnx/hr/hr_char_pinyin.txt";
            if (!string.Equals(configured.Replace('\\', '/'), legacy, StringComparison.OrdinalIgnoreCase))
                pinyin = PinyinTable.Get(SherpaNativeOptions.ResolveAsset(legacy));
        }
        if (pinyin is null) return null;   // 字典缺失 → 退化为仅通用数字同音归一（PinyinTable 已打日志）

        var aligner = new SoundAligner(pinyin, rows, columnCount)
        {
            Threshold = configuration.GetValue("Align:Threshold", 0.75),
            Margin = configuration.GetValue("Align:Margin", 0.15),
        };
        return aligner.Ready ? aligner : null;
    }
}

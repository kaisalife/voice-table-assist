using System.Text;

namespace VoiceTableAssist.Asr;

/// <summary>
/// 按"导入的表格模板"生成语音特化资源（纯文本、由表驱动，零人工维护）：
///   hotwords.txt —— 送入 sherpa-onnx --hotwords-file（解码 bias），含行标签 + 列描述符 + 单字数字。
/// 表内同音纠错不再落地规则文件：由 <see cref="SoundAligner"/>（表内读音吸附，词表 = 行标签 + 列说法）
/// 在识别文本上按读音吸附，覆盖原 hr_rules（只认拼音完全相同）的全部能力，且换表自动生效。
/// </summary>
internal static class TableVoiceResourceGenerator
{
    private static readonly string[] ChDigits =
        ["零", "一", "二", "三", "四", "五", "六", "七", "八", "九", "十"];

    /// <summary>
    /// 参与热词加权的单字数字：**刻意不含"十"**。
    /// "十"与"点"在同一段声学上是竞争候选（「二号一点二」会被听成「二号十二」）：
    /// 给"十"同样 bonus 会把"一+点"路径压掉，"点"就丢了（18 条小数用例 A/B：含十 14/18 → 去十 18/18；
    /// 五十/十五点二/二十/一百 等含"十"读法无回归，靠声学即可读对）。
    /// "十"的同音（实/石/时）由 <see cref="DomainCorrection"/> 在文本层归一，不依赖热词。
    /// </summary>
    private static readonly string[] HotwordDigits =
        ["零", "一", "二", "三", "四", "五", "六", "七", "八", "九"];

    public static string ToChineseNum(int n)
    {
        if (n <= 0) return n.ToString();
        if (n <= 10) return ChDigits[n];
        if (n < 20) return "十" + (n % 10 == 0 ? "" : ChDigits[n - 10]);
        var tens = n / 10;
        var ones = n % 10;
        return ChDigits[tens] + "十" + (ones > 0 ? ChDigits[ones] : "");
    }

    /// <summary>
    /// 固定列描述符短语族（测量值X / X号 / 第X个 / 第X列 / 序号X / X号列）。
    /// 与行标签无关，始终按 columnCount 生成，避免语音读错；同时作为表内读音吸附的位置词表。
    /// </summary>
    public static IReadOnlyList<string> ColumnDescriptors(int columnCount)
    {
        var list = new List<string>();
        for (var c = 1; c <= columnCount; c++)
        {
            var zh = ToChineseNum(c);
            var ar = c.ToString();
            list.Add($"{zh}号");    // 一号..六号
            list.Add($"{ar}号");    // 1号..6号
            list.Add($"第{zh}个");  // 第一个..第六个
            list.Add($"第{ar}个");  // 第1个..第6个
            list.Add($"第{zh}列");  // 第一列..第六列
            list.Add($"第{ar}列");  // 第1列..第6列
            list.Add($"测量值{zh}"); // 测量值一..测量值六
            list.Add($"测量值{ar}"); // 测量值1..测量值6
            list.Add($"序号{zh}");   // 序号一..序号六
            list.Add($"序号{ar}");   // 序号1..序号6
            list.Add($"{zh}号列");   // 一号列..六号列
            list.Add($"{ar}号列");   // 1号列..6号列
        }
        return list;
    }

    /// <summary>
    /// 生成热词文本。rows 为行标签（检验内容）；columnCount 为列数。
    /// sherpa 热词按"字级 token"解析：每个汉字之间必须用空格分隔（如「一 号」），整行连写映射不到 token。
    /// 模型词表无 ASCII 数字，识别也不可能输出「1号」——含非汉字字符的短语直接跳过。
    /// 无论何种表，都会固定加入 X号/第X个/第X列 等列描述符短语与单字数字（不含"十"，见 HotwordDigits）。
    /// </summary>
    public static string BuildHotWords(IReadOnlyList<string> rows, int columnCount)
    {
        var sb = new StringBuilder();
        void AppendPhrase(string phrase)
        {
            // 热词行：字级 token 空格分隔；词表只有汉字，含非汉字（ASCII 数字/字母）的短语模型输出不了，跳过
            if (phrase.Length == 0 || phrase.Any(c => c is not (>= '\u4E00' and <= '\u9FFF'))) return;
            sb.AppendLine(string.Join(' ', phrase.ToCharArray()));
        }

        foreach (var r in rows)
            AppendPhrase(r.Trim());

        foreach (var desc in ColumnDescriptors(columnCount))
            AppendPhrase(desc);

        // 单字数字 / 小数点加权，对抗同音（如 五→武）；"十"刻意不加权（见 HotwordDigits）
        foreach (var ch in HotwordDigits) sb.AppendLine(ch);
        sb.AppendLine("点");
        return sb.ToString();
    }

    /// <summary>把生成的热词写到磁盘（按表目录隔离）。仅供运维/排障查看——
    /// 运行时热词按流经 <see cref="BuildHotWordsStream"/> 直传 sherpa，不再从文件加载。</summary>
    public static void WriteHotWords(string hotWordsText, string tableDir)
    {
        Directory.CreateDirectory(tableDir);
        File.WriteAllText(Path.Combine(tableDir, "hotwords.txt"), hotWordsText, new UTF8Encoding(false));
    }

    /// <summary>
    /// 生成**按流传入**的热词串：内容与 <see cref="BuildHotWords"/> 完全一致，
    /// 只是把多行用 '/' 连成一条串（sherpa <c>CreateStream(hotwords)</c> 的格式），
    /// 供每通语音会话绑定"本表词表"——识别器常驻，切表/导入都不重建、不重启。
    /// </summary>
    public static string BuildHotWordsStream(IReadOnlyList<string> rows, int columnCount)
    {
        var text = BuildHotWords(rows, columnCount);
        var parts = text.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        return string.Join('/', parts);
    }

    /// <summary>
    /// 依据配置重建语音资源（hotwords.txt，仅供运维查看）到指定表目录（tables/{key}，default→current 兼容）。
    /// 运行时热词按流传入，因此这里**不需要**任何进程重启。
    /// </summary>
    public static (bool Ok, string? Error, string? TableDir) Rebuild(
        IConfiguration configuration,
        IReadOnlyList<string> rows,
        int columnCount,
        string? tableKey = null)
    {
        static string Resolve(IConfiguration cfg, string key, string def)
        {
            var p = string.IsNullOrWhiteSpace(cfg[key]) ? def : cfg[key]!;
            return Path.IsPathRooted(p) ? p : Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, p));
        }

        var currentDir = Resolve(configuration, "Tables:HrCurrentDir", "sherpa-onnx/hr/tables/current");

        // 目标目录：default 表 → tables/current（历史路径，兼容旧部署）；其余 → tables/{key}
        var tableDir = KeyIsDefault(tableKey)
            ? currentDir
            : Path.Combine(Resolve(configuration, "Tables:HrBaseDir", "sherpa-onnx/hr/tables"), tableKey!);

        try
        {
            WriteHotWords(BuildHotWords(rows, columnCount), tableDir);
            return (true, null, tableDir);
        }
        catch (Exception ex)
        {
            return (false, ex.Message, tableDir);
        }
    }

    private static bool KeyIsDefault(string? key) =>
        string.IsNullOrWhiteSpace(key) || string.Equals(key, "default", StringComparison.OrdinalIgnoreCase);
}

using System.Text;

namespace VoiceTableAssist.Asr;

/// <summary>
/// 按"导入的表格模板"生成两块语音特化资源（纯文本、由表驱动，替代原 hardcode 的 hotwords/规则）：
///  1. hotwords.txt    —— 送入 sherpa-onnx --hotwords-file（解码 bias），含行标签 + 列描述符；
///  2. hr_rules.txt    —— 送入 HomophoneReplacer（拼音=汉字），把识别文本里的同音错字纠正为本表行标签。
/// 均无需编译 FST / 无需 Python，运行期仅由 .NET 生成。
/// </summary>
internal static class TableVoiceResourceGenerator
{
    private static readonly string[] ChDigits =
        ["零", "一", "二", "三", "四", "五", "六", "七", "八", "九", "十"];

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
    /// 与行标签无关，始终按 columnCount 生成，避免语音读错。
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
    /// 无论何种表，都会固定加入 X号/第X个/第X列 等列描述符短语与单字数字。
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

        // 单字数字 / 小数点加权，对抗同音（如 五→武）
        foreach (var ch in ChDigits) sb.AppendLine(ch);
        sb.AppendLine("点");
        sb.AppendLine("零");
        return sb.ToString();
    }

    /// <summary>
    /// 生成同音替换规则文本（每行 拼音=汉字）。给每个行标签生成"正确拼音=正确标签"；
    /// 若 columnCount&gt;0，额外固定加入列描述符（测量值X/X号/第X个/第X列/序号X/X号列）的恒等规则，
    /// 使 ASR 把它们读成同音错字时能纠正回目标写法（需要拼音表含对应同音字）。
    /// 最后追加 commonRulesPath 的跨表通用近音规则。
    /// </summary>
    public static string BuildRules(
        HomophoneReplacer lexicon,
        IReadOnlyList<string> rows,
        string? commonRulesPath = null,
        int columnCount = 0)
    {
        var seen = new HashSet<string>(StringComparer.Ordinal);
        var sb = new StringBuilder();
        foreach (var r in rows)
        {
            var label = r.Trim();
            if (label.Length == 0) continue;
            var key = lexicon.ToTone3Pinyin(label);
            if (key.Length == 0 || seen.Contains(key)) continue;
            seen.Add(key);
            sb.Append(key).Append('=').Append(label).AppendLine();
        }

        // 固定列描述符恒等规则：拼音=目标写法
        foreach (var desc in ColumnDescriptors(columnCount))
        {
            var key = lexicon.ToTone3Pinyin(desc);
            if (key.Length == 0 || seen.Contains(key)) continue;
            seen.Add(key);
            sb.Append(key).Append('=').Append(desc).AppendLine();
        }

        if (!string.IsNullOrEmpty(commonRulesPath) && File.Exists(commonRulesPath))
            foreach (var line in File.ReadLines(commonRulesPath, Encoding.UTF8))
                if (!string.IsNullOrWhiteSpace(line))
                    sb.AppendLine(line.Trim());

        return sb.ToString();
    }

    /// <summary>把生成内容写到磁盘（按表目录隔离）。</summary>
    public static void Write(string hotWordsText, string rulesText, string tableDir)
    {
        Directory.CreateDirectory(tableDir);
        File.WriteAllText(Path.Combine(tableDir, "hotwords.txt"), hotWordsText, new UTF8Encoding(false));
        File.WriteAllText(Path.Combine(tableDir, "hr_rules.txt"), rulesText, new UTF8Encoding(false));
    }

    /// <summary>
    /// 依据配置重建语音资源（hotwords.txt + hr_rules.txt）到指定表目录（tables/{key}，default→tables/current 兼容）。
    /// 供 /import_table（进程内直调）与 /api/table/voice 共用。
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

        var charPinyin = Resolve(configuration, "Homophone:CharPinyin", "sherpa-onnx/hr/hr_char_pinyin.txt");
        var common = Resolve(configuration, "Homophone:CommonRules", "sherpa-onnx/hr/hr_common_rules.txt");

        // 目标目录：default 表 → 旧 tables/current（向后兼容）；其余 → tables/{key}
        var tableDir = KeyIsDefault(tableKey)
            ? Resolve(configuration, "Homophone:TableDir", "sherpa-onnx/hr/tables/current")
            : Path.Combine(Resolve(configuration, "Tables:HrBaseDir", "sherpa-onnx/hr/tables"), tableKey!);

        // 热词始终生成；hr_rules（同音纠正）仅在拼音表可用时生成，缺失时只走热词加权
        var hotWords = BuildHotWords(rows, columnCount);
        if (File.Exists(charPinyin))
        {
            var rules = BuildRules(new HomophoneReplacer(charPinyin), rows, common, columnCount);
            Write(hotWords, rules, tableDir);
        }
        else
        {
            Directory.CreateDirectory(tableDir);
            File.WriteAllText(Path.Combine(tableDir, "hotwords.txt"), hotWords, new UTF8Encoding(false));
        }

        // sherpa 仅在进程启动时读一次热词文件，且只会加载 tables/current/hotwords.txt 一个文件——
        // 把所有已导入表的热词聚合写入该文件（去重），配合导入后的进程重启，切表时解码偏置始终覆盖全部行标签。
        AggregateHotwords(Path.GetDirectoryName(tableDir)!, Resolve(configuration, "Homophone:TableDir", "sherpa-onnx/hr/tables/current"));

        return (true, null, tableDir);
    }

    /// <summary>合并 tables/ 下所有表的 hotwords.txt（去重）写入 sherpa 启动加载的 current 目录。</summary>
    private static void AggregateHotwords(string tablesRoot, string currentDir)
    {
        var merged = new SortedSet<string>(StringComparer.Ordinal);
        if (Directory.Exists(tablesRoot))
        {
            foreach (var file in Directory.EnumerateFiles(tablesRoot, "hotwords.txt", SearchOption.AllDirectories))
                foreach (var line in File.ReadLines(file, Encoding.UTF8))
                {
                    var s = line.Trim();
                    if (s.Length > 0 && !s.StartsWith('#')) merged.Add(s);
                }
        }
        Directory.CreateDirectory(currentDir);
        File.WriteAllText(Path.Combine(currentDir, "hotwords.txt"), string.Concat(merged.Select(l => l + "\n")), new UTF8Encoding(false));
    }

    private static bool KeyIsDefault(string? key) =>
        string.IsNullOrWhiteSpace(key) || string.Equals(key, "default", StringComparison.OrdinalIgnoreCase);
}
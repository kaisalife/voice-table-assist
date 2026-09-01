namespace VoiceTableAssist.Asr;

/// <summary>
/// 提供/缓存 HomophoneReplacer 实例。紧凑拼音表（hr_char_pinyin.txt，仓库自带，无需通用词典）全局按需加载一次；
/// 规则文件按表切分（hr_rules.txt），当文件变化（导入新表）时自动重建。
/// tableKey：default/null → 旧 tables/current 规则（向后兼容）；其余 → tables/{key}。
/// </summary>
internal static class HomophoneReplacerProvider
{
    private static readonly object Gate = new();
    private static HomophoneReplacer? _cached;
    private static string? _cachedRulesPath;
    private static DateTime _cachedRulesMtime;
    private static string? _cachedCharPinyinPath;

    public static HomophoneReplacer? Get(IConfiguration configuration, string? tableKey = null)
    {
        var charPinyin = Resolve(configuration["Homophone:CharPinyin"], AppContext.BaseDirectory,
            "sherpa-onnx/hr/hr_char_pinyin.txt");
        if (string.IsNullOrWhiteSpace(charPinyin) || !File.Exists(charPinyin)) return null;

        var rulesPath = KeyIsDefault(tableKey)
            ? Resolve(configuration["Homophone:TableRules"], AppContext.BaseDirectory,
                "sherpa-onnx/hr/tables/current/hr_rules.txt")
            : Resolve(null, AppContext.BaseDirectory, $"sherpa-onnx/hr/tables/{tableKey}/hr_rules.txt");
        var commonPath = Resolve(configuration["Homophone:CommonRules"], AppContext.BaseDirectory,
            "sherpa-onnx/hr/hr_common_rules.txt");

        var rulesMtime = rulesPath != null && File.Exists(rulesPath) ? File.GetLastWriteTimeUtc(rulesPath) : DateTime.MinValue;

        lock (Gate)
        {
            if (_cached != null
                && string.Equals(_cachedCharPinyinPath, charPinyin, StringComparison.OrdinalIgnoreCase)
                && string.Equals(_cachedRulesPath, rulesPath, StringComparison.OrdinalIgnoreCase)
                && _cachedRulesMtime == rulesMtime)
                return _cached;

            var replacer = new HomophoneReplacer(charPinyin, ReadLines(rulesPath).Concat(ReadLines(commonPath)));
            if (!replacer.Enabled) return null;

            _cached = replacer;
            _cachedCharPinyinPath = charPinyin;
            _cachedRulesPath = rulesPath;
            _cachedRulesMtime = rulesMtime;
            return _cached;
        }
    }

    private static IEnumerable<string> ReadLines(string? path)
    {
        if (string.IsNullOrEmpty(path) || !File.Exists(path)) yield break;
        foreach (var line in File.ReadLines(path)) yield return line;
    }

    private static string? Resolve(string? cfg, string baseDir, string def)
    {
        var p = string.IsNullOrWhiteSpace(cfg) ? def : cfg;
        if (string.IsNullOrWhiteSpace(p)) return null;
        return Path.IsPathRooted(p) ? p : Path.GetFullPath(Path.Combine(baseDir, p));
    }

    private static bool KeyIsDefault(string? key) =>
        string.IsNullOrWhiteSpace(key) || string.Equals(key, "default", StringComparison.OrdinalIgnoreCase);
}
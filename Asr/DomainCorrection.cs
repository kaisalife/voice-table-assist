namespace VoiceTableAssist.Asr;

/// <summary>
/// 领域后处理纠错引擎。
/// 在 ASR 输出与 NER 推理之间插入，修正通用 ASR 模型在巡检场景下的系统性同音词错误。
/// 分两层：直接替换（无歧义）和上下文替换（需判断上下文）。
/// </summary>
internal static class DomainCorrection
{
    /// <summary>
    /// 对 ASR 识别文本执行领域纠错，返回修正后的文本。
    /// </summary>
    public static string Correct(string text)
    {
        if (string.IsNullOrWhiteSpace(text)) return text;

        // 第一层：直接替换（无歧义，领域词汇唯一）
        text = ApplyDirectReplacements(text);

        // 第二层：上下文替换（需判断数字/列号上下文）
        text = ApplyContextAwareReplacements(text);

        return text;
    }

    // ========== 第一层：直接替换 ==========

    private static readonly Dictionary<string, string> DirectReplacements = new()
    {
        // 行标签同音词
        ["印度"] = "硬度",
        ["外景"] = "外径",
        ["外经"] = "外径",
        ["外净"] = "外径",
        ["外静"] = "外径",
        ["内景"] = "内径",
        ["内经"] = "内径",
        ["内净"] = "内径",
        ["表面光洁"] = "表面光洁度",
        ["表面关节"] = "表面光洁度",
        ["表面光洁毒"] = "表面光洁度",
        ["员工"] = "圆度",
        ["零度"] = "圆度",

        // 测量值客体
        ["测量直"] = "测量值",
        ["测量之"] = "测量值",
    };

    private static string ApplyDirectReplacements(string text)
    {
        foreach (var (wrong, right) in DirectReplacements)
            text = text.Replace(wrong, right);
        return text;
    }

    // ========== 第二层：上下文替换 ==========

    /// <summary>
    /// 每个条目为一个替换规则：(pattern, replacement)。
    /// pattern 是正则表达式，replacement 是替换后的文本。
    /// </summary>
    private static readonly (string Pattern, string Replacement)[] ContextRules =
    {
        // "付" 后面跟数字或"点" → "负"（负值场景）
        // 例："付五点零" → "负五点零", "付零点五" → "负零点五"
        (@"付(?=[\d一二三四五六七八九十零点百])", "负"),

        // "灵" 在数字上下文中 → "零"
        // 例："灵点零一" → "零点零一", "一灵" → "一零"
        (@"(?<=[\d一二三四五六七八九十百])灵(?=[\d一二三四五六七八九十点百])", "零"),
        (@"灵(?=[\d一二三四五六七八九十点百])", "零"),

        // "实" 在数字上下文中 → "十"
        // 例："五实" → "五十", "实二" → "十二"
        (@"(?<=[\d一二三四五六七八九])实(?=[\d一二三四五六七八九点])", "十"),
        (@"实(?=[\d一二三四五六七八九])", "十"),

        // "好" 在列号/序号上下文中 → "号"
        // 例："二好" → "二号", "一好列" → "一号列", "测量值一好" → "测量值一号"
        (@"(?<=[一二三四五六七八九十])好(?=列|$|，|,)", "号"),

        // "月" 在约数上下文中 → "约"
        // 例："月八十三点四零" → "约八十三点四零"
        (@"(约|月)(?=[\d一二三四五六七八九十])", "约"),
    };

    private static string ApplyContextAwareReplacements(string text)
    {
        foreach (var (pattern, replacement) in ContextRules)
        {
            text = System.Text.RegularExpressions.Regex.Replace(text, pattern, replacement);
        }
        return text;
    }
}
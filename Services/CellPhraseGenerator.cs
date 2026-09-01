namespace VoiceTableAssist.Services;

/// <summary>
/// 为表格每个单元格生成所有可能的自然语言指代短语。
/// 用于向量库初始化，覆盖用户口述时可能使用的各种说法。
/// </summary>
public static class CellPhraseGenerator
{
    private static readonly string[] ChDigits = ["零", "一", "二", "三", "四", "五", "六", "七", "八", "九", "十"];

    /// <summary>阿拉伯数字 → 中文数字（1~99）。</summary>
    public static string ToChineseNum(int n)
    {
        if (n <= 0) return n.ToString();
        if (n <= 10) return ChDigits[n];
        if (n < 20) return "十" + (n % 10 == 0 ? "" : ChDigits[n - 10]);
        int tens = n / 10, ones = n % 10;
        return ChDigits[tens] + "十" + (ones > 0 ? ChDigits[ones] : "");
    }

    /// <summary>
    /// 为单元格 (rowIdx, colIdx) 生成所有指代短语。
    /// 覆盖：[行标签/序号几/第几行] + [几号/第几个/第几列/测量值几] 的全部排列组合。
    /// 只用中文数字——模型词表没有 ASCII 数字，语音识别不可能输出「1号」，阿拉伯变体是纯冗余条目。
    /// </summary>
    public static List<string> Generate(int rowIdx, string rowLabel, int colIdx)
    {
        var cnRow = ToChineseNum(rowIdx);
        var cnCol = ToChineseNum(colIdx);

        // 行描述符：行标签 + 序号几 + 第几行
        var rowDescs = new List<string>
        {
            rowLabel,
            $"序号{cnRow}",
            $"第{cnRow}行",
        };

        // 列描述符：几号 + 第几个 + 第几列 + 测量值几
        var colDescs = new List<string>
        {
            $"{cnCol}号",
            $"第{cnCol}个",
            $"第{cnCol}列",
            $"测量值{cnCol}",
        };

        // 全部排列：行描述符 + 列描述符，以及反过来的列描述符 + 行描述符；
        // 「测量值几」不反序——「测量值二振动」不是自然说法，反序条目只会稀释检索
        var phrases = new HashSet<string>();
        foreach (var rd in rowDescs)
        {
            foreach (var cd in colDescs)
            {
                phrases.Add(rd + cd);
                if (!cd.StartsWith("测量值")) phrases.Add(cd + rd);
            }
        }

        return phrases.ToList();
    }
}
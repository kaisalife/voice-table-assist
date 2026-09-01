using Microsoft.ML.OnnxRuntime;
using Microsoft.ML.OnnxRuntime.Tensors;
using System.Text;
using System.Text.Json;

namespace VoiceTableAssist.Services;

/// <summary>
/// 中文数字(正则取数) -> 阿拉伯数字，范围 0~1000，保留两位小数。
/// 从原 text_to_json Program.cs 提取为共享工具，供 /text_to_json 与 /api/speech/ner 使用。
/// </summary>
public static class ChineseNumeral
{
    public static double ToDecimal(string raw)
    {
        var m = System.Text.RegularExpressions.Regex.Match(
            raw, "负?[零〇一二三四五六七八九十百千点。]+");
        if (!m.Success) return 0;
        var s = m.Value;
        var neg = s.StartsWith('负');          // 「负九十五」→ -95（巡检负压/真空场景）
        if (neg) s = s[1..];
        var dig = new Dictionary<char, int>
        {
            ['零'] = 0, ['〇'] = 0, ['一'] = 1, ['二'] = 2, ['三'] = 3,
            ['四'] = 4, ['五'] = 5, ['六'] = 6, ['七'] = 7, ['八'] = 8, ['九'] = 9,
        };

        var dot = s.IndexOfAny(new[] { '点', '。' });
        var intPart = dot >= 0 ? s[..dot] : s;

        // 中文整数累加解析（支持 千/百/十/个，最大到"一千"=1000）
        int result = 0, hold = 0;
        foreach (var c in intPart)
        {
            if (dig.TryGetValue(c, out var d)) { hold = d; continue; }
            var now = hold == 0 ? 1 : hold;              // "十二"→十位补1
            switch (c)
            {
                case '十': result += now * 10; hold = 0; break;
                case '百': result += now * 100; hold = 0; break;
                case '千': result += now * 1000; hold = 0; break;
            }
        }
        result += hold;
        if (result < 0 || result > 1000) return 0;

        double v = result;
        if (dot >= 0 && dot < s.Length - 1)
        {
            var frac = s[(dot + 1)..];
            double f = 0;
            for (var k = 0; k < Math.Min(frac.Length, 2); k++)
            {
                var d = dig.GetValueOrDefault(frac[k]);
                f += d * (k == 0 ? 0.1 : 0.01);
            }
            v += f;
        }
        if (neg) v = -v;
        return Math.Round(v, 2);
    }
}
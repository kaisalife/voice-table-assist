using System.Linq;
using System.Text;

namespace VoiceTableAssist.Asr;

/// <summary>
/// 原生 C# 实现的同音字/专名替换器，复刻 sherpa-onnx HomophoneReplacer 的语义（免 pynini / 免 replace.fst 二进制）：
/// 1) 用"内置紧凑 汉字->带调拼音"表（hr_char_pinyin.txt，只需覆盖本表行标签出现的汉字）把识别文本逐字转拼音；
/// 2) 在整串拼音上做"整串拼音=汉字"的短语替换（等价 cdrewrite 行为），未命中部分原样保留（等价 FST 恒等）。
///
/// 键 = 误识别文本的拼音（TONE3，低声用1声，无空格）；值 = 正确汉字（通常为本表行标签）。
/// </summary>
internal sealed class HomophoneReplacer
{
    private readonly Dictionary<char, string> _charPinyin = new();
    // 规则：整串拼音 -> 汉字
    private readonly Dictionary<string, string> _rules = new(StringComparer.Ordinal);

    /// <param name="charPinyinPath">每行 "汉字=拼音"（或空格分隔）的紧凑拼音表。</param>
    /// <param name="ruleLines">规则（每行 拼音=汉字），可选。</param>
    public HomophoneReplacer(string charPinyinPath, IEnumerable<string>? ruleLines = null)
    {
        foreach (var line in File.ReadLines(charPinyinPath, Encoding.UTF8))
        {
            var s = line.Trim();
            if (s.Length == 0 || s.StartsWith('#')) continue;
            string? pinyin = null;
            var eq = s.IndexOf('=');
            if (eq > 0)
            {
                var ch = s[..eq].Trim();
                pinyin = s[(eq + 1)..].Trim();
                if (ch.Length == 1 && pinyin.Length > 0) _charPinyin[ch[0]] = pinyin;
            }
            else
            {
                var parts = s.Split(' ', '\t');
                if (parts.Length >= 2 && parts[0].Length == 1)
                    _charPinyin[parts[0][0]] = string.Concat(parts.Skip(1));
            }
        }

        if (ruleLines != null)
        {
            foreach (var line in ruleLines)
            {
                var s = line.Trim();
                if (s.Length == 0 || s.StartsWith('#')) continue;
                var eq = s.IndexOf('=');
                if (eq <= 0) continue;
                var key = s[..eq].Trim();
                var val = s[(eq + 1)..].Trim();
                if (key.Length > 0 && val.Length > 0)
                    _rules[key] = val;
            }
        }
    }

    /// <summary>是否有可用规则/拼音表（用于判定是否启用纠正）。</summary>
    public bool Enabled => _charPinyin.Count > 0;

    private static bool IsHan(char c) => c >= 0x4E00 && c <= 0x9FFF;

    /// <summary>对 ASR 识别文本执行同音字/专名替换，返回纠正后的文本。</summary>
    public string Apply(string text)
    {
        if (string.IsNullOrWhiteSpace(text) || _rules.Count == 0) return text;

        var tokens = Tokenize(text);
        var n = tokens.Count;
        var isHanTok = new bool[n];
        var pinyin = new string[n];
        var starts = new int[n];
        var lens = new int[n];

        for (var idx = 0; idx < n; idx++)
        {
            var (st, ln, ch) = tokens[idx];
            starts[idx] = st;
            lens[idx] = ln;
            if (ch.All(IsHan))
            {
                isHanTok[idx] = true;
                pinyin[idx] = ToPinyinChar(ch[0]);
            }
        }

        var output = new List<string>(text.Length);
        var i = 0;
        while (i < n)
        {
            if (!isHanTok[i]) { output.Add(text.Substring(starts[i], lens[i])); i++; continue; }

            // 从 i 开始跨连续的汉字 token 累积拼音，找到最长命中的规则键
            var acc = new StringBuilder();
            var j = i;
            string? target = null;
            var end = -1;
            while (j < n && isHanTok[j])
            {
                acc.Append(pinyin[j]);
                var key = acc.ToString();
                if (_rules.TryGetValue(key, out var t))
                {
                    target = t;
                    end = j;
                    var hasLonger = _rules.Keys.Any(k => k.Length > key.Length && k.StartsWith(key, StringComparison.Ordinal));
                    if (!hasLonger) break;
                }
                j++;
            }

            if (target != null && end >= i)
            {
                output.Add(target);
                i = end + 1;
            }
            else
            {
                output.Add(text.Substring(starts[i], lens[i]));
                i++;
            }
        }

        return string.Concat(output);
    }

    /// <summary>把一个汉字串转成无空格带调拼音串（逐字查表）。用于为本表行标签生成规则键。</summary>
    public string ToTone3Pinyin(string chinese)
    {
        var sb = new StringBuilder();
        foreach (var c in chinese)
            sb.Append(_charPinyin.TryGetValue(c, out var p) ? p : c.ToString());
        return sb.ToString();
    }

    /// <summary>逐字转拼音；无表回退原字。</summary>
    private string ToPinyinChar(char c) => _charPinyin.TryGetValue(c, out var p) ? p : c.ToString();

    /// <summary>分词：每个汉字为独立 token；连续非汉字为一段 token（作匹配边界）。</summary>
    private List<(int Start, int Len, string Chr)> Tokenize(string text)
    {
        var result = new List<(int, int, string)>();
        var n = text.Length;
        var i = 0;
        while (i < n)
        {
            if (!IsHan(text[i]))
            {
                var j = i;
                while (j < n && !IsHan(text[j])) j++;
                result.Add((i, j - i, text.Substring(i, j - i)));
                i = j;
            }
            else
            {
                result.Add((i, 1, text[i].ToString()));
                i += 1;
            }
        }
        return result;
    }
}
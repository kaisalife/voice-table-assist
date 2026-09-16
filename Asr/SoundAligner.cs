namespace VoiceTableAssist.Asr;

/// <summary>
/// 表内读音吸附器（对齐《新语音输入同音解决方案.md》与 Android 版 jni/text/sound_aligner.cc）。
///
/// 目标：把 ASR 听错的"主体/位置"按【读音】吸附到当前表的词表上，零人工维护。
///   词表来源（全部自动）：行标签（表本身） + 列说法（由列数枚举，见 TableVoiceResourceGenerator.ColumnDescriptors）
///   音近模型（与表无关的通用规则，硬编码一次）：
///     忽略声调 / 前鼻音后鼻音不分 / 平舌翘舌不分 / n-l、f-h 不分 / 音节编辑距离≤1
///   防误纠守卫：相似度阈值 + 与次优候选的差距 + 音节数一致 → 分不清就放弃（宁缺勿错填）。
///
/// 用于喂 NER 之前（RaNER 用正确行名训练），partial 与 final 都纠（界面所见=所填）。
/// </summary>
internal sealed class SoundAligner
{
    private static readonly string[] Initials2 = ["zh", "ch", "sh"];
    private static readonly char[] Initials1 =
        ['b', 'p', 'm', 'f', 'd', 't', 'n', 'l', 'g', 'k', 'h', 'j', 'q', 'x', 'r', 'z', 'c', 's', 'y', 'w'];

    private readonly PinyinTable _pinyin;
    private readonly List<Entry> _words = new();
    private readonly List<string> _vocabText = new();

    /// <summary>采纳阈值（默认 0.75）。</summary>
    public double Threshold { get; set; } = 0.75;

    /// <summary>与次优候选的最小差距（默认 0.15）。</summary>
    public double Margin { get; set; } = 0.15;

    /// <summary>发生替换时的回调（原文、规范写法、相似度）；供调用方打日志。</summary>
    public Action<string, string, double>? OnReplace { get; set; }

    /// <param name="pinyin">已加载的去声调拼音字典。</param>
    /// <param name="rowLabels">本表行标签。</param>
    /// <param name="columnCount">列数（用于枚举位置词表）。</param>
    public SoundAligner(PinyinTable pinyin, IReadOnlyList<string> rowLabels, int columnCount)
    {
        _pinyin = pinyin;

        void AddWord(string raw)
        {
            var word = raw.Trim();
            if (word.Length == 0) return;
            // 词表只收纯汉字（含 ASCII 数字/单位的写法 ASR 不会输出，且无读音可比）
            foreach (var c in word)
                if (!IsHan(c)) return;
            foreach (var e in _words)
                if (e.Text == word) return;   // 去重

            var syllables = _pinyin.Syllables(word);
            if (syllables.Length == 0) return;
            foreach (var s in syllables)
                if (s.Length == 0) return;    // 有字不在字典里 → 该词不可用（避免半截匹配）

            _words.Add(new Entry(word, word.ToCharArray(), syllables));
        }

        foreach (var r in rowLabels) AddWord(r);
        if (columnCount > 0)
            foreach (var d in TableVoiceResourceGenerator.ColumnDescriptors(columnCount)) AddWord(d);

        foreach (var e in _words) _vocabText.Add(e.Text);
    }

    /// <summary>拼音字典与词表均可用时才启用。</summary>
    public bool Ready => _words.Count > 0;

    /// <summary>词表（行标签 + 位置词），便于现场核对"到底吸附了哪些词"。</summary>
    public IReadOnlyList<string> Vocabulary => _vocabText;

    /// <summary>吸附结果：Canonical 为空 = 未采纳（分不清 → 放弃）。</summary>
    public readonly record struct Match(string Canonical, double Score, double Margin)
    {
        public static Match Rejected => new("", 0, 0);
    }

    /// <summary>整词吸附：在词表里找读音最像的词；不达阈值/分不清 → Canonical 为空。</summary>
    public Match AlignWord(string word)
    {
        if (!Ready) return Match.Rejected;
        var w = (word ?? "").Trim();
        if (w.Length == 0) return Match.Rejected;

        // 已经就是表内词 → 无需吸附
        foreach (var e in _words)
            if (e.Text == w) return new Match(w, 1.0, 1.0);

        var syllables = _pinyin.Syllables(w);
        if (syllables.Length == 0) return Match.Rejected;
        foreach (var s in syllables)
            if (s.Length == 0) return Match.Rejected;   // 有字不在字典里 → 不猜

        var (bestText, best, second) = TopTwo(syllables);
        var margin = best - second;
        if (best >= Threshold && margin >= Margin && bestText.Length > 0)
            return new Match(bestText, best, margin);
        return new Match("", best, margin);
    }

    /// <summary>
    /// 整句吸附：把句中高置信的片段替换为词表规范写法，其余原样保留。
    /// 用于喂 NER 之前：先把错字纠回正字，NER 才抽得出主体/位置。
    /// </summary>
    public string AlignSentence(string text)
    {
        if (!Ready || string.IsNullOrEmpty(text)) return text;

        var cps = text.ToCharArray();
        var n = cps.Length;

        // 保护区：与词表**精确相等**的片段一律不参与替换
        // （否则"一号"会被读音相近的"四号"替换——yihao/sihao 音节编辑距离 1）
        var protectedAt = new int[n];
        foreach (var e in _words)
        {
            var len = e.Chars.Length;
            if (len <= 0 || len > n) continue;
            for (var i = 0; i + len <= n; i++)
            {
                if (!cps.AsSpan(i, len).SequenceEqual(e.Chars)) continue;
                for (var k = i; k < i + len; k++) protectedAt[k] = len;
            }
        }

        var candidates = new List<Candidate>();
        foreach (var e in _words)
        {
            var len = e.Syllables.Length;
            if (len <= 0 || len > n) continue;
            for (var i = 0; i + len <= n; i++)
            {
                if (protectedAt[i] != 0) continue;            // 该处已是表内词（精确）→ 保护区
                if (!AllHan(cps, i, len)) continue;
                if (AllNumeral(cps, i, len)) continue;        // 值段不动

                var spanSyl = new string[len];
                var anyMissing = false;
                for (var k = 0; k < len; k++)
                {
                    var s = _pinyin.Syllable(cps[i + k]);
                    if (s.Length == 0) { anyMissing = true; break; }
                    spanSyl[k] = s;
                }
                if (anyMissing) continue;

                var score = SequenceSim(spanSyl, e.Syllables);
                if (score < Threshold) continue;

                var span = new string(cps, i, len);
                if (span == e.Text) continue;                 // 本来就对，不用替换
                candidates.Add(new Candidate(i, len, score, e.Text, span));
            }
        }
        if (candidates.Count == 0) return text;

        // 同一片段（start,len）取前二名做差距守卫
        candidates.Sort((a, b) =>
        {
            if (a.Start != b.Start) return a.Start.CompareTo(b.Start);
            if (a.Len != b.Len) return b.Len.CompareTo(a.Len);
            return b.Score.CompareTo(a.Score);
        });

        var accepted = new List<Candidate>();
        for (var i = 0; i < candidates.Count;)
        {
            var j = i;
            double second = 0;
            while (j < candidates.Count && candidates[j].Start == candidates[i].Start
                                        && candidates[j].Len == candidates[i].Len)
            {
                if (j > i) second = Math.Max(second, candidates[j].Score);
                j++;
            }
            var top = candidates[i];
            if (top.Score - second >= Margin) accepted.Add(top);
            i = j;
        }
        if (accepted.Count == 0) return text;

        // 全局贪心：高分优先、长词优先，避免重叠替换
        accepted.Sort((a, b) => a.Score != b.Score ? b.Score.CompareTo(a.Score) : b.Len.CompareTo(a.Len));
        var used = new bool[n];
        var replOf = new int[n];
        Array.Fill(replOf, -1);
        var chosen = new bool[accepted.Count];
        for (var k = 0; k < accepted.Count; k++)
        {
            var overlap = false;
            for (var t = accepted[k].Start; t < accepted[k].Start + accepted[k].Len; t++)
            {
                if (used[t] || protectedAt[t] != 0) { overlap = true; break; }
            }
            if (overlap) continue;
            for (var t = accepted[k].Start; t < accepted[k].Start + accepted[k].Len; t++) used[t] = true;
            replOf[accepted[k].Start] = k;
            chosen[k] = true;
        }

        // 重建文本
        var sb = new System.Text.StringBuilder(n);
        for (var i = 0; i < n;)
        {
            var k = replOf[i];
            if (k >= 0 && chosen[k])
            {
                sb.Append(accepted[k].Canonical);
                OnReplace?.Invoke(accepted[k].Original, accepted[k].Canonical, accepted[k].Score);
                i += accepted[k].Len;
            }
            else
            {
                sb.Append(cps[i]);
                i++;
            }
        }
        return sb.ToString();
    }

    // ---- 音近模型（与表无关的通用规则）----

    /// <summary>音节相似度：相同=1.0；声母等价且韵母（去鼻音）相同=0.9；音节编辑距离≤1=0.6；否则 0。</summary>
    public static double SyllableSim(string a, string b)
    {
        if (string.IsNullOrEmpty(a) || string.IsNullOrEmpty(b)) return 0;
        if (a == b) return 1.0;

        var (ai, af) = SplitSyllable(a);
        var (bi, bf) = SplitSyllable(b);
        if (NormalizeInitial(ai) == NormalizeInitial(bi) && NormalizeFinal(af) == NormalizeFinal(bf))
            return 0.9;
        if (EditDistance(a, b) <= 1) return 0.6;
        return 0;
    }

    /// <summary>音节序列相似度：音节数必须一致，任一对音节完全不像则整体不认。</summary>
    public static double SequenceSim(IReadOnlyList<string> a, IReadOnlyList<string> b)
    {
        if (a.Count == 0 || b.Count == 0 || a.Count != b.Count) return 0;
        double sum = 0;
        for (var i = 0; i < a.Count; i++)
        {
            var s = SyllableSim(a[i], b[i]);
            if (s <= 0) return 0;
            sum += s;
        }
        return sum / a.Count;
    }

    /// <summary>声母拆解（按长度优先匹配）；零声母（a/o/e 开头）返回空声母。</summary>
    private static (string Initial, string Final) SplitSyllable(string syllable)
    {
        foreach (var ini in Initials2)
        {
            if (syllable.Length > 2 && syllable.AsSpan(0, 2).SequenceEqual(ini)) return (syllable[..2], syllable[2..]);
        }
        if (syllable.Length > 1 && Array.IndexOf(Initials1, syllable[0]) >= 0)
            return (syllable[..1], syllable[1..]);
        return ("", syllable);
    }

    /// <summary>声母等价类：平翘舌、n-l、f-h。</summary>
    private static string NormalizeInitial(string initial) => initial switch
    {
        "zh" => "z",
        "ch" => "c",
        "sh" => "s",
        "n" => "l",
        "f" => "h",
        _ => initial,
    };

    /// <summary>前鼻音/后鼻音不分（末尾 ng ↔ n）。</summary>
    private static string NormalizeFinal(string final)
        => final.Length >= 2 && final.EndsWith("ng", StringComparison.Ordinal) ? final[..^1] : final;

    private static int EditDistance(string a, string b)
    {
        var n = a.Length;
        var m = b.Length;
        if (n == 0) return m;
        if (m == 0) return n;
        var prev = new int[m + 1];
        var cur = new int[m + 1];
        for (var j = 0; j <= m; j++) prev[j] = j;
        for (var i = 1; i <= n; i++)
        {
            cur[0] = i;
            for (var j = 1; j <= m; j++)
            {
                var cost = a[i - 1] == b[j - 1] ? 0 : 1;
                cur[j] = Math.Min(Math.Min(prev[j] + 1, cur[j - 1] + 1), prev[j - 1] + cost);
            }
            (prev, cur) = (cur, prev);
        }
        return prev[m];
    }

    private static bool IsHan(char c) => c >= 0x4E00 && c <= 0x9FFF;

    private static bool AllHan(char[] cps, int start, int len)
    {
        for (var i = start; i < start + len; i++)
            if (!IsHan(cps[i])) return false;
        return true;
    }

    private static bool AllNumeral(char[] cps, int start, int len)
    {
        for (var i = start; i < start + len; i++)
            if (!IsNumeral(cps[i])) return false;
        return true;
    }

    private static bool IsNumeral(char c) => c switch
    {
        '零' or '〇' or '一' or '二' or '三' or '四' or '五' or '六' or '七' or '八' or '九'
            or '十' or '百' or '千' or '点' or '。' or '负' => true,
        _ => c is >= '0' and <= '9',
    };

    /// <summary>命中前二名（best/second），用于阈值 + 差距守卫。</summary>
    private (string BestText, double Best, double Second) TopTwo(IReadOnlyList<string> spanSyllables)
    {
        var bestText = "";
        double best = 0, second = 0;
        foreach (var e in _words)
        {
            if (e.Syllables.Length != spanSyllables.Count) continue;
            var score = SequenceSim(spanSyllables, e.Syllables);
            if (score > best)
            {
                second = best;
                best = score;
                bestText = e.Text;
            }
            else if (score > second)
            {
                second = score;
            }
        }
        return (bestText, best, second);
    }

    private readonly record struct Entry(string Text, char[] Chars, string[] Syllables);

    private readonly record struct Candidate(int Start, int Len, double Score, string Canonical, string Original);
}

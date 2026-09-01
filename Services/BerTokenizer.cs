using System.Text;
using System.Text.Json;

namespace VoiceTableAssist.Services;

// =====================================================================
// gte-base-zh = BERT tokenizer（do_lower_case + tokenize_chinese_chars）
// 直接以 tokenizer.json 的 model.vocab 建词表，复刻 BERT basic+wordpiece。
// =====================================================================
public static class BerTokenizer
{
    public const int CLS = 101, SEP = 102, PAD = 0, UNK = 100;

    private static readonly HashSet<char> _cjk = new(Enumerable.Range(0x4E00, 0x9FA5 - 0x4E00 + 1).Select(c => (char)c));
    private static readonly Dictionary<string, int> _special = new() { { "[CLS]", CLS }, { "[SEP]", SEP }, { "[PAD]", PAD }, { "[UNK]", UNK } };

    public static TokenizerData Load(string path)
    {
        using var doc = JsonDocument.Parse(File.ReadAllBytes(path));
        var vocab = new Dictionary<string, int>();
        foreach (var p in doc.RootElement.GetProperty("model").GetProperty("vocab").EnumerateObject())
            vocab.TryAdd(p.Name, p.Value.GetInt32());
        return new TokenizerData(vocab);
    }

    public static (long[] ids, long[] mask, long[] types) Encode(TokenizerData t, string text, int maxLen = 256)
    {
        var tokens = new List<int> { CLS };
        foreach (var word in BasicTokenize(text))
        {
            if (t.Vocab.TryGetValue(word, out var id)) { tokens.Add(id); continue; }
            foreach (var piece in WordPiece(t, word)) tokens.Add(piece);
        }
        tokens.Add(SEP);
        if (tokens.Count > maxLen - 1) tokens = tokens.GetRange(0, maxLen - 1).Concat(new[] { SEP }).ToList();

        var ids = new long[maxLen];
        var mask = new long[maxLen];
        for (int i = 0; i < tokens.Count; i++) { ids[i] = tokens[i]; mask[i] = 1; }
        return (ids, mask, new long[maxLen]);
    }

    private static IEnumerable<string> BasicTokenize(string text)
    {
        var sb = new StringBuilder();
        foreach (var ch in text)
        {
            if (_cjk.Contains(ch)) sb.Append(' ').Append(ch).Append(' ');
            else sb.Append(ch);
        }
        foreach (var raw in sb.ToString().ToLowerInvariant().Split(new[] { ' ', '\t', '\n', '\r' }, StringSplitOptions.RemoveEmptyEntries))
            yield return raw;
    }

    private static IEnumerable<int> WordPiece(TokenizerData t, string word)
    {
        if (t.Vocab.TryGetValue(word, out var id)) { yield return id; yield break; }
        var start = 0;
        while (start < word.Length)
        {
            int end = word.Length;
            int found = -1;
            while (end > start)
            {
                var sub = (start > 0 ? "##" : "") + word[start..end];
                if (t.Vocab.TryGetValue(sub, out var v)) { found = v; break; }
                end--;
            }
            if (found >= 0) { yield return found; start = end; }
            else { yield return UNK; start++; }
        }
    }
}

public sealed record TokenizerData(Dictionary<string, int> Vocab)
{
    public int VocabSize => Vocab.Count;
}
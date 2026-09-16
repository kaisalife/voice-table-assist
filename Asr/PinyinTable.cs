using System.Text;

namespace VoiceTableAssist.Asr;

/// <summary>
/// 汉字→去声调拼音（通用字典，随包内置，与表无关）。对齐 Android 版 jni/text/pinyin_table.cc。
/// 文件格式：每行 "汉字=ying4"（TONE3，音节末尾数字）；加载时统一剥掉声调数字，得到去声调形式（ying），
/// 供表内读音吸附（<see cref="SoundAligner"/>）使用；旧格式（空格分隔）也兼容。
/// 进程内按路径缓存，避免多会话重复加载 0.4MB 字典。
/// </summary>
internal sealed class PinyinTable
{
    private static readonly object Gate = new();
    private static readonly Dictionary<string, PinyinTable?> Cache = new(StringComparer.OrdinalIgnoreCase);

    private readonly Dictionary<char, string> _map = new();

    public int Count => _map.Count;

    /// <summary>按路径取单例（加载失败返回 null）。</summary>
    public static PinyinTable? Get(string? charPinyinPath)
    {
        if (string.IsNullOrWhiteSpace(charPinyinPath)) return null;
        lock (Gate)
        {
            if (Cache.TryGetValue(charPinyinPath, out var cached)) return cached;
            var table = Load(charPinyinPath);
            Cache[charPinyinPath] = table;
            return table;
        }
    }

    private static PinyinTable? Load(string path)
    {
        if (!File.Exists(path))
        {
            Console.WriteLine($"[PY] 拼音表不存在: {path}");
            return null;
        }

        var table = new PinyinTable();
        foreach (var raw in File.ReadLines(path, Encoding.UTF8))
        {
            var line = raw.Trim();
            if (line.Length == 0 || line[0] == '#') continue;

            string ch;
            string pinyin;
            var eq = line.IndexOf('=');
            if (eq > 0)
            {
                ch = line[..eq].Trim();
                pinyin = line[(eq + 1)..].Trim();
            }
            else
            {
                // 兼容空格分隔：首列单字，其余列拼接
                var parts = line.Split([' ', '\t'], StringSplitOptions.RemoveEmptyEntries);
                if (parts.Length < 2) continue;
                ch = parts[0];
                pinyin = string.Concat(parts.Skip(1));
            }

            if (ch.Length != 1 || pinyin.Length == 0) continue;
            table._map[ch[0]] = StripTone(pinyin);
        }

        if (table._map.Count == 0)
        {
            Console.WriteLine($"[PY] 拼音表为空或无法解析: {path}");
            return null;
        }
        Console.WriteLine($"[PY] 拼音表加载完成: {table._map.Count} 字 ({path})");
        return table;
    }

    /// <summary>"ying4" → "ying"；"lv4" → "lv"；顺带去掉末尾轻声标记。</summary>
    private static string StripTone(string syllable)
    {
        var end = syllable.Length;
        while (end > 0 && syllable[end - 1] >= '0' && syllable[end - 1] <= '9') end--;
        return syllable[..end];
    }

    /// <summary>单字去声调音节（如 硬→ying、一→yi）；非汉字/未收录返回空串。</summary>
    public string Syllable(char c) => _map.TryGetValue(c, out var p) ? p : "";

    /// <summary>整串逐字音节（非汉/未收录 → 空串占位，长度与字符数一致）。</summary>
    public string[] Syllables(string text)
    {
        var result = new string[text.Length];
        for (var i = 0; i < text.Length; i++) result[i] = Syllable(text[i]);
        return result;
    }
}

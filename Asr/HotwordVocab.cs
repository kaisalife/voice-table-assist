namespace VoiceTableAssist.Asr;

/// <summary>
/// 热词可用字表：从识别模型的 tokens.txt 读出**单字 token** 集合，用来判断一条热词短语能否被
/// 完整编码进解码图。
///
/// 背景（对齐 sherpa-onnx v1.13.6 源码 utils.cc:EncodeBase）：
///   sherpa 按空格逐 token 查表，查不到只打日志（has_oov=true），**该短语仍会被塞进热词图，
///   但只剩查得到的字**——例如「外 径」会退化成只 boost 单字「外」，「光 洁 度」退化成伪词
///   「光度」。这是往解码里塞错误偏置，比没有热词更糟。
///
/// 因此本项目的策略（见《新语音输入同音解决方案》）：**短语必须能完整编码才加载为热词**，
/// 否则整条不加载——这类行名（外径/内径/光洁度…）交给后续的**表内读音吸附 + 数字同音归一**兜底，
/// 识别文本与交互完全不受影响。
///
/// 说明：本仓库 ASR 模型为 BBPE 2000 词表（含 256 个 &lt;0xNN&gt; 字节 token + 多字片段），
/// 单字 token 才可直接用于 cjkchar 模式；字节回退/多字片段都不算"完整可编码"。
/// </summary>
internal sealed class HotwordVocab
{
    private static readonly object Gate = new();
    private static readonly Dictionary<string, HotwordVocab?> Cache = new(StringComparer.OrdinalIgnoreCase);

    private readonly HashSet<char> _chars = new();

    public int Count => _chars.Count;

    /// <summary>按 tokens.txt 路径取（进程内缓存；文件缺失返回 null=不过滤）。</summary>
    public static HotwordVocab? Get(string? tokensPath)
    {
        if (string.IsNullOrWhiteSpace(tokensPath)) return null;
        lock (Gate)
        {
            if (Cache.TryGetValue(tokensPath, out var cached)) return cached;
            var vocab = Load(tokensPath);
            Cache[tokensPath] = vocab;
            return vocab;
        }
    }

    private static HotwordVocab? Load(string path)
    {
        if (!File.Exists(path))
        {
            Console.WriteLine($"[HOTWORD] tokens.txt 不存在（热词不做可用性过滤）: {path}");
            return null;
        }

        var vocab = new HotwordVocab();
        foreach (var raw in File.ReadLines(path))
        {
            var line = raw.Trim();
            if (line.Length == 0) continue;
            var sp = line.IndexOf(' ');
            var token = sp > 0 ? line[..sp] : line;
            if (token.Length == 1) vocab._chars.Add(token[0]);   // 只收单字 token（字节/多字片段不可用）
        }

        if (vocab._chars.Count == 0) return null;
        Console.WriteLine($"[HOTWORD] 热词可用单字 token: {vocab._chars.Count} 个 ({Path.GetFileName(path)})");
        return vocab;
    }

    /// <summary>该短语是否能被完整编码（全部字都在单字 token 表里）。</summary>
    public bool CanEncode(string phrase)
    {
        if (string.IsNullOrEmpty(phrase)) return false;
        foreach (var c in phrase)
            if (!_chars.Contains(c)) return false;
        return true;
    }
}

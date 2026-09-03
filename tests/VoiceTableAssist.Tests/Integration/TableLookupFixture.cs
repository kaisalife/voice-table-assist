using VoiceTableAssist.Services;
using Xunit;

namespace VoiceTableAssist.Tests.Integration;

/// <summary>
/// 集成测试 fixture：在测试类启动时一次性加载 RaNER + gte-base-zh 两个 ONNX 引擎、
/// 构建一张小业务表（6 行 × 4 列）并把每个单元格的全排列短语落进 <see cref="VectorIndexData"/>。
/// 所有测试共享同一份索引，跑完整个类再 Dispose。
/// </summary>
public sealed class TableLookupFixture : IAsyncLifetime
{
    /// <summary>与生产默认一致；测试内个别用例会临时调低看 raw similarity。</summary>
    private const float MinSim = 0.0f;

    public EmbeddingEngine Embed { get; private set; } = null!;
    public RaNerEngine Raner { get; private set; } = null!;
    public VectorIndexData Index { get; private set; } = null!;

    /// <summary>本 fixture 用的"模拟业务表"行标签（典型力学性能表）。</summary>
    public static readonly string[] BusinessRowLabels =
        ["外径", "内径", "厚度", "抗拉强度", "屈服强度", "延伸率"];

    public const int BusinessColumnCount = 4;

    public async Task InitializeAsync()
    {
        var (ranerDir, embedDir) = ResolveModelDirs();
        EnsureDir(ranerDir, "raner");
        EnsureDir(embedDir, "embedding");

        // 两个引擎各自加载（~2-5s 一次性成本）
        Raner = new RaNerEngine(ranerDir);
        Embed = new EmbeddingEngine(embedDir) { MinSim = MinSim };

        // 走一遍生产同款的"导入"流程：CellPhraseGenerator → EmbedBatch → VectorIndexData
        Index = await Task.Run(BuildBusinessIndex);
    }

    public Task DisposeAsync()
    {
        Embed?.Dispose();
        Raner?.Dispose();
        return Task.CompletedTask;
    }

    /// <summary>
    /// 复刻 <see cref="TableVectorManager.Import"/> 的纯数据路径：
    ///   for each (row, col): phrases = CellPhraseGenerator.Generate(row, label, col);
    ///   EmbedBatch(phrases) → 一组向量
    ///   拼成 (row, col, phrase, vec) entries
    /// 不写盘、不触发语音资源、不调度 sherpa 重启——只测 embedding 准确性。
    /// </summary>
    private VectorIndexData BuildBusinessIndex()
    {
        var allPhrases = new List<(int Row, int Col, string Phrase)>();
        for (var row = 1; row <= BusinessRowLabels.Length; row++)
        {
            for (var col = 1; col <= BusinessColumnCount; col++)
                foreach (var p in CellPhraseGenerator.Generate(row, BusinessRowLabels[row - 1], col))
                    allPhrases.Add((row, col, p));
        }

        // 批量嵌入（一次 ONNX 推理一批），与生产路径一致
        const int batchSize = 64;
        var entries = new List<Cell>(allPhrases.Count);
        for (var i = 0; i < allPhrases.Count; i += batchSize)
        {
            var batch = allPhrases.GetRange(i, Math.Min(batchSize, allPhrases.Count - i));
            var texts = batch.Select(b => b.Phrase).ToList();
            var vecs = Embed.EmbedBatch(texts);
            for (var j = 0; j < batch.Count; j++)
                entries.Add(new Cell(batch[j].Row, batch[j].Col, batch[j].Phrase, vecs[j]));
        }

        var dim = entries.Count > 0 ? entries[0].Vec.Length : 0;
        return new VectorIndexData(entries, BusinessRowLabels, BusinessRowLabels.Length, BusinessColumnCount, dim);
    }

    private static (string RanerDir, string EmbedDir) ResolveModelDirs()
    {
        // 优先用环境变量；否则从测试 bin 目录回溯到仓库根的 models/ 目录
        var envRoot = Environment.GetEnvironmentVariable("RANER_MODEL_DIR");
        string root;
        if (!string.IsNullOrWhiteSpace(envRoot))
        {
            root = Path.GetFullPath(envRoot);
        }
        else
        {
            // test bin: <repo>/tests/VoiceTableAssist.Tests/bin/Debug/net8.0/
            // 上溯 4 层到 <repo>/，再接 models/
            var baseDir = AppContext.BaseDirectory;
            root = Path.GetFullPath(Path.Combine(baseDir, "..", "..", "..", "..", "..", "models"));
        }
        return (Path.Combine(root, "raner"), Path.Combine(root, "embedding"));
    }

    private static void EnsureDir(string dir, string name)
    {
        if (!Directory.Exists(dir))
            throw new FileNotFoundException(
                $"模型目录 {name} 不存在：{dir}\n" +
                "请将模型放到 <repo>/models/ 下，或设置 RANER_MODEL_DIR 环境变量。");
        var required = name == "raner"
            ? new[] { "model.onnx", "vocab.txt", "crf_transitions.json" }
            : new[] { "model_quantized.onnx", "tokenizer.json" };
        foreach (var f in required)
        {
            var p = Path.Combine(dir, f);
            if (!File.Exists(p))
                throw new FileNotFoundException(
                    $"{name} 模型缺失文件：{p}\n请补齐后重跑。");
        }
    }
}

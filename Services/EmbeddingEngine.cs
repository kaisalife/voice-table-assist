using Microsoft.ML.OnnxRuntime;
using Microsoft.ML.OnnxRuntime.Tensors;
using System.Text;

namespace VoiceTableAssist.Services;

/// <summary>
/// gte-base-zh 嵌入引擎（纯推理组件，可随 EngineHost 按需加载/卸载）：
///  1. 加载 model_quantized.onnx + tokenizer.json（线程安全、进程内共享、不随表重建）；
///  2. 活动向量索引由 TableVectorManager 持有，查询一律显式传入（Lookup(phrase, index)）；
///  3. Query(phrase) -> attention-mask 加权 mean 池化 + normalize -> 与库向量余弦 -> 最近邻 (row,col)
///     （gte 系模型官方推荐 mean 池化，CLS 池化会损失区分度；池化方式变更后向量库需重建）。
/// 移植自 test/embedding/gte-base-zh/CsColdStart。
/// </summary>
public sealed class EmbeddingEngine : IDisposable
{
    private readonly InferenceSession _session;
    private readonly string _lastHiddenName;
    private readonly TokenizerData _tok;
    private readonly string[] _inputNames;

    /// <summary>最近邻余弦相似度低于该值视为未命中（返回 row=-1）；0=不过滤。由配置 Embedding:MinSim 设置。</summary>
    public float MinSim { get; set; }

    public EmbeddingEngine(string modelDir)
    {
        string tokPath = Path.Combine(modelDir, "tokenizer.json");
        string onnxPath = Path.Combine(modelDir, "model_quantized.onnx");
        _tok = BerTokenizer.Load(tokPath);
        _session = new InferenceSession(onnxPath, new Microsoft.ML.OnnxRuntime.SessionOptions());
        _inputNames = _session.InputMetadata.Keys.ToArray();
        _lastHiddenName = _session.OutputMetadata.Keys
            .FirstOrDefault(k => k.Contains("last_hidden_state", StringComparison.OrdinalIgnoreCase))
            ?? _session.OutputMetadata.Keys.First();
    }

    public float[] Embed(string text)
    {
        var (ids, mask, types) = BerTokenizer.Encode(_tok, text, maxLen: 256);
        var seq = ids.Length;
        var inputs = new List<NamedOnnxValue>();
        if (_inputNames.Contains("input_ids"))
            inputs.Add(NamedOnnxValue.CreateFromTensor("input_ids", new DenseTensor<long>(ids, new[] { 1, seq })));
        if (_inputNames.Contains("attention_mask"))
            inputs.Add(NamedOnnxValue.CreateFromTensor("attention_mask", new DenseTensor<long>(mask, new[] { 1, seq })));
        if (_inputNames.Contains("token_type_ids"))
            inputs.Add(NamedOnnxValue.CreateFromTensor("token_type_ids", new DenseTensor<long>(types, new[] { 1, seq })));

        using var output = _session.Run(inputs);
        var hidden = output.First(o => o.Name == _lastHiddenName).AsTensor<float>();
        return Normalize(MeanPool(hidden, mask, batch: 0));
    }

    /// <summary>批量嵌入：将多条文本在一次 ONNX 推理中完成，大幅减少推理调用次数。</summary>
    public List<float[]> EmbedBatch(IReadOnlyList<string> texts, int maxLen = 256)
    {
        if (texts.Count == 0) return new List<float[]>();

        // 1. 逐条编码，记录最大长度
        var encoded = new List<(long[] ids, long[] mask, long[] types)>(texts.Count);
        int maxSeq = 0;
        foreach (var text in texts)
        {
            var e = BerTokenizer.Encode(_tok, text, maxLen);
            var len = e.ids.Count(id => id != 0); // 实际非 padding 长度
            if (len > maxSeq) maxSeq = len;
            encoded.Add(e);
        }
        maxSeq = Math.Min(maxSeq, maxLen);

        // 2. 构建 batch 张量 [batch_size, maxSeq]
        int batchSize = texts.Count;
        var idsBatch = new long[batchSize * maxSeq];
        var maskBatch = new long[batchSize * maxSeq];
        var typesBatch = new long[batchSize * maxSeq];

        for (int b = 0; b < batchSize; b++)
        {
            var (ids, mask, types) = encoded[b];
            for (int s = 0; s < maxSeq; s++)
            {
                int idx = b * maxSeq + s;
                idsBatch[idx] = s < ids.Length ? ids[s] : 0;
                maskBatch[idx] = s < mask.Length ? mask[s] : 0;
                typesBatch[idx] = s < types.Length ? types[s] : 0;
            }
        }

        // 3. 运行 ONNX
        var inputs = new List<NamedOnnxValue>();
        if (_inputNames.Contains("input_ids"))
            inputs.Add(NamedOnnxValue.CreateFromTensor("input_ids", new DenseTensor<long>(idsBatch, new[] { batchSize, maxSeq })));
        if (_inputNames.Contains("attention_mask"))
            inputs.Add(NamedOnnxValue.CreateFromTensor("attention_mask", new DenseTensor<long>(maskBatch, new[] { batchSize, maxSeq })));
        if (_inputNames.Contains("token_type_ids"))
            inputs.Add(NamedOnnxValue.CreateFromTensor("token_type_ids", new DenseTensor<long>(typesBatch, new[] { batchSize, maxSeq })));

        using var output = _session.Run(inputs);
        var hidden = output.First(o => o.Name == _lastHiddenName).AsTensor<float>();

        // 4. 逐样本 mean 池化（attention-mask 加权）并归一化
        var results = new List<float[]>(batchSize);
        for (int b = 0; b < batchSize; b++)
        {
            var sliceMask = new long[maxSeq];
            for (int s = 0; s < maxSeq; s++) sliceMask[s] = maskBatch[b * maxSeq + s];
            results.Add(Normalize(MeanPool(hidden, sliceMask, b)));
        }
        return results;
    }

    /// <summary>
    /// 按指定表的索引查询（会话级快照，不依赖全局活动表）：
    /// 语音会话提交时用连接建立时刻的索引，连接 A/B 并发提交互不串表。
    /// 最近邻相似度低于 <see cref="MinSim"/> 时返回 row=-1（未命中），调用方应跳过该格。
    /// </summary>
    public (int Row, int Col, float Sim, string Phrase) Lookup(string phrase, VectorIndexData? index)
    {
        var idx = index; // 索引不可变，查询期间切表不影响本快照
        if (idx is null || idx.Entries.Count == 0) return (-1, -1, 0f, "");
        var clean = CleanPhrase(phrase);
        var q = Embed(clean);
        var best = idx.Entries[0];
        float bestSim = float.MinValue;
        foreach (var e in idx.Entries)
        {
            var s = Cosine(q, e.Vec);
            if (s > bestSim) { bestSim = s; best = e; }
        }
        if (bestSim < MinSim) return (-1, -1, bestSim, best.Phrase);   // 低置信：宁缺勿错填
        return (best.Row, best.Col, bestSim, best.Phrase);
    }

    /// <summary>与 mjs embedQuery 一致：去掉中文/半角逗号与空白。</summary>
    public static string CleanPhrase(string phrase)
    {
        var sb = new StringBuilder(phrase.Length);
        foreach (var ch in phrase)
        {
            if (ch is '，' or ',' or '、' or ' ' or '\t' or '\n' or '\r') continue;
            sb.Append(ch);
        }
        return sb.ToString();
    }

    private static float[] Normalize(float[] v)
    {
        double s = 0;
        foreach (var x in v) s += x * x;
        var n = Math.Sqrt(s);
        if (n > 0) for (int i = 0; i < v.Length; i++) v[i] = (float)(v[i] / n);
        return v;
    }

    /// <summary>attention-mask 加权 mean 池化：gte 系模型的官方句向量方式（取 batch 批次 b）。</summary>
    private float[] MeanPool(Tensor<float> hidden, long[] mask, int batch)
    {
        int dim = hidden.Dimensions[^1];
        int seq = hidden.Dimensions[1];
        var vec = new float[dim];
        double denom = 0;
        for (int s = 0; s < seq; s++)
        {
            if (s >= mask.Length || mask[s] == 0) continue;
            denom += 1;
            for (int d = 0; d < dim; d++) vec[d] += hidden[batch, s, d];
        }
        if (denom > 0) for (int d = 0; d < dim; d++) vec[d] = (float)(vec[d] / denom);
        return vec;
    }

    private static float Cosine(float[] a, float[] b)
    {
        double d = 0;
        for (int i = 0; i < a.Length; i++) d += a[i] * b[i];
        return (float)d;
    }

    public void Dispose() => _session.Dispose();
}
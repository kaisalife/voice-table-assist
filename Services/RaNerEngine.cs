using Microsoft.ML.OnnxRuntime;
using Microsoft.ML.OnnxRuntime.Tensors;
using System.Text.Json;

namespace VoiceTableAssist.Services;

/// <summary>
/// RaNER（BERT+CRF NER）引擎：
///  使用 exprotv2 的 model.onnx，逐字 tokenizer（vocab.txt），MAX_LEN=128，
///  输出 emissions 接 Viterbi（crf_transitions.json）得到 BIO 标签，再提取三元组。
/// 复刻自 test/RaNER/to_index/test_embed.mjs。
/// </summary>
public sealed class RaNerEngine
{
    public const int MAX_LEN = 128;
    public static readonly string[] LABELS = { "O", "B-SUB", "I-SUB", "B-OBJ", "I-OBJ", "B-VAL", "I-VAL" };
    public const int CLS = 101, SEP = 102, PAD = 0, UNK = 100;

    private readonly Dictionary<string, int> _vocab;
    private readonly float[] _transitions; // [7*7] 平铺 CRF 转移矩阵
    private readonly InferenceSession _session;
    private readonly string[] _inputNames;
    private readonly string _emissionsName;
    private readonly int _numLabels = LABELS.Length;

    public RaNerEngine(string modelDir)
    {
        string vocabPath = Path.Combine(modelDir, "vocab.txt");
        string onnxPath = Path.Combine(modelDir, "model.onnx");
        string crfPath = Path.Combine(modelDir, "crf_transitions.json");

        _vocab = LoadVocab(vocabPath);
        _transitions = LoadTransitions(crfPath);
        _session = new InferenceSession(onnxPath, new Microsoft.ML.OnnxRuntime.SessionOptions());
        _inputNames = _session.InputMetadata.Keys.ToArray();
        _emissionsName = _session.OutputMetadata.Keys
            .FirstOrDefault(k => k.Contains("emission", StringComparison.OrdinalIgnoreCase))
            ?? _session.OutputMetadata.Keys.First();
    }

    private static Dictionary<string, int> LoadVocab(string path)
    {
        var vocab = new Dictionary<string, int>();
        var lines = File.ReadAllLines(path);
        for (int i = 0; i < lines.Length; i++)
        {
            var t = lines[i].Trim();
            if (t.Length > 0 && !vocab.ContainsKey(t)) vocab[t] = i;
        }
        return vocab;
    }

    private static float[] LoadTransitions(string path)
    {
        using var doc = JsonDocument.Parse(File.ReadAllBytes(path));
        var root = doc.RootElement;
        // 兼容扁平数组、嵌套数组或 {"shape":..,"data":[..]}
        if (root.ValueKind == JsonValueKind.Array && root.GetArrayLength() > 0 && root[0].ValueKind == JsonValueKind.Number)
            return root.EnumerateArray().Select(e => (float)e.GetDouble()).ToArray();
        if (root.ValueKind == JsonValueKind.Array)
        {
            var flat = new List<float>();
            foreach (var arr in root.EnumerateArray())
                foreach (var x in arr.EnumerateArray()) flat.Add((float)x.GetDouble());
            return flat.ToArray();
        }
        return root.GetProperty("data").EnumerateArray().Select(e => (float)e.GetDouble()).ToArray();
    }

    // ---------- 逐字 tokenizer ----------
    private (long[] ids, long[] mask, int[] wordIds) Tokenize(string text)
    {
        var chars = text.ToCharArray();
        var ids = new List<long> { CLS };
        var wordIds = new List<int?> { null };
        for (int i = 0; i < chars.Length; i++)
        {
            ids.Add(_vocab.TryGetValue(chars[i].ToString(), out var id) ? id : UNK);
            wordIds.Add(i);
        }
        ids.Add(SEP);
        wordIds.Add(null);
        var idArr = new long[MAX_LEN];
        var maskArr = new long[MAX_LEN];
        var wIdx = new int[MAX_LEN];
        Array.Fill(wIdx, -1);
        for (int i = 0; i < ids.Count && i < MAX_LEN; i++) { idArr[i] = ids[i]; maskArr[i] = 1; if (wordIds[i] is int w) wIdx[i] = w; }
        return (idArr, maskArr, wIdx);
    }

    // ---------- 推理 ----------
    public List<(string Ch, string Tag)> Predict(string text)
    {
        var (ids, mask, wIdx) = Tokenize(text);
        var seq = ids.Length;
        var inputs = new List<NamedOnnxValue>();
        if (_inputNames.Contains("input_ids"))
            inputs.Add(NamedOnnxValue.CreateFromTensor("input_ids", new DenseTensor<long>(ids, new[] { 1, seq })));
        if (_inputNames.Contains("attention_mask"))
            inputs.Add(NamedOnnxValue.CreateFromTensor("attention_mask", new DenseTensor<long>(mask, new[] { 1, seq })));

        using var output = _session.Run(inputs);
        var em = output.First(o => o.Name == _emissionsName).AsTensor<float>();
        // emissions: [1, MAX_LEN, numLabels]，平铺索引 = t*numLabels + j
        int flatLen = (int)em.Length;
        var emissions = new float[flatLen];
        for (int i = 0; i < flatLen; i++) emissions[i] = em.GetValue(i);

        var pred = Viterbi(emissions, mask);
        return CollapseToWords(pred, wIdx, text);
    }

    // ---------- Viterbi ----------
    private int[] Viterbi(float[] emissions, long[] mask)
    {
        int seqLen = mask.Length;
        var score = new float[seqLen * _numLabels];
        var back = new int[seqLen * _numLabels];

        for (int j = 0; j < _numLabels; j++) score[j] = emissions[j];

        for (int t = 1; t < seqLen; t++)
        {
            for (int j = 0; j < _numLabels; j++)
            {
                if (mask[t] == 0)
                {
                    score[t * _numLabels + j] = score[(t - 1) * _numLabels + j];
                    back[t * _numLabels + j] = j;
                    continue;
                }
                float best = float.NegativeInfinity;
                int bestK = 0;
                for (int k = 0; k < _numLabels; k++)
                {
                    float s = score[(t - 1) * _numLabels + k] + _transitions[k * _numLabels + j];
                    if (s > best) { best = s; bestK = k; }
                }
                score[t * _numLabels + j] = emissions[t * _numLabels + j] + best;
                back[t * _numLabels + j] = bestK;
            }
        }

        // 回溯
        var path = new int[seqLen];
        int bestLast = 0;
        float bestScore = float.NegativeInfinity;
        for (int j = 0; j < _numLabels; j++)
        {
            if (score[(seqLen - 1) * _numLabels + j] > bestScore) { bestScore = score[(seqLen - 1) * _numLabels + j]; bestLast = j; }
        }
        path[seqLen - 1] = bestLast;
        for (int t = seqLen - 2; t >= 0; t--) path[t] = back[(t + 1) * _numLabels + path[t + 1]];
        return path;
    }

    // 把 word 级别标签折叠回字符（去重连续相同 word idx 的 padding）
    private List<(string, string)> CollapseToWords(int[] pred, int[] wIdx, string text)
    {
        var chars = text.ToCharArray();
        var result = new List<(string, string)>();
        int prevWordIdx = -1;
        for (int i = 0; i < wIdx.Length; i++)
        {
            int widx = wIdx[i];
            if (widx < 0 || widx == prevWordIdx) continue;
            result.Add((chars[widx].ToString(), LABELS[pred[i]]));
            prevWordIdx = widx;
        }
        return result;
    }

    public void Dispose() => _session.Dispose();
}
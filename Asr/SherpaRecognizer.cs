using System.Runtime.InteropServices;

namespace VoiceTableAssist.Asr;

/// <summary>
/// 进程内 sherpa-onnx 流式识别器宿主（替代原 sherpa-onnx-online-websocket-server 子进程）：
///   - 识别器（模型）进程内常驻、随服务启动后台加载；不随语义引擎空闲卸载；
///   - 热词按流传入：每通会话 <see cref="CreateStream"/> 传本表热词串 → 切表/导入**无需重建识别器**；
///   - 每连接一个 <see cref="SherpaStream"/>（解码状态），互不影响。
/// 线程模型：识别器创建/销毁加锁；多个 stream 各自单线程使用（本项目单连接，天然满足）。
/// </summary>
internal sealed class SherpaRecognizerHost : IHostedService, IDisposable
{
    private readonly SherpaNativeOptions _options;
    private readonly ILogger<SherpaRecognizerHost> _logger;
    private readonly SemaphoreSlim _loadLock = new(1, 1);
    private readonly object _gate = new();

    private IntPtr _recognizer = IntPtr.Zero;
    private volatile bool _ready;
    private string? _error;

    public SherpaRecognizerHost(IConfiguration configuration, ILogger<SherpaRecognizerHost> logger)
    {
        _options = SherpaNativeOptions.From(configuration);
        _logger = logger;
    }

    /// <summary>
    /// 启动即预载 sherpa 原生库（**必须先于其它 ONNX 运行时**，见 Program.cs 调用点注释）：
    /// sherpa 自带 onnxruntime 1.27，而本项目 ONNX 依赖是 Microsoft.ML.OnnxRuntime 1.20；
    /// Windows 按模块名复用已加载模块，若 1.20 先入进程，sherpa-c-api 会绑定到 1.20 而加载失败。
    /// ORT C API 向后兼容（旧 API 版本可在新运行时上取到），故让 1.27 先入进程。
    /// </summary>
    public static bool TryPreloadNative(IConfiguration configuration, ILogger logger)
    {
        try
        {
            var dir = SherpaNativeOptions.From(configuration).ResolveNativeDir();
            if (!Directory.Exists(dir))
            {
                logger.LogWarning("[ASR] 未找到 sherpa 原生库目录 {Dir}（语音识别不可用）", dir);
                return false;
            }
            SherpaNative.Load(dir);
            var version = Marshal.PtrToStringUTF8(SherpaNative.SherpaOnnxGetVersionStr()) ?? "?";
            logger.LogInformation("[ASR] 已预载 sherpa-onnx 原生库 {Version}（{Dir}），热词按流传入", version, dir);
            return true;
        }
        catch (Exception ex)
        {
            logger.LogWarning(ex, "[ASR] 预载 sherpa 原生库失败（语音识别不可用）");
            return false;
        }
    }

    /// <summary>识别器是否已创建完成（模型加载完毕）。</summary>
    public bool IsReady => _ready && _recognizer != IntPtr.Zero;

    /// <summary>最近一次加载失败原因（健康检查/排障用）。</summary>
    public string? LastError => _error;

    /// <summary>原生 sherpa-onnx 版本（加载后可得，未加载为 "?"）。</summary>
    public string Version { get; private set; } = "?";

    public int SampleRate => _options.SampleRate;

    /// <summary>按流传词（'/'）创建解码流；空串/空值 = 不带热词。</summary>
    public SherpaStream CreateStream(string? hotwords)
    {
        if (!IsReady) throw new InvalidOperationException("语音识别引擎未就绪（应先调用 EnsureReadyAsync）");
        return new SherpaStream(_recognizer, _options.SampleRate, hotwords);
    }

    /// <summary>确保识别器已加载（单飞：并发调用共享同一次加载；失败可重试）。</summary>
    public async Task EnsureReadyAsync(Action<string>? progress = null)
    {
        if (IsReady) return;
        await _loadLock.WaitAsync().ConfigureAwait(false);
        try
        {
            if (IsReady) return;
            progress?.Invoke("正在加载语音识别引擎（首次使用需数秒）...");
            await Task.Run(CreateRecognizer).ConfigureAwait(false);
            _ready = true;
            _error = null;
        }
        finally { _loadLock.Release(); }
    }

    private void CreateRecognizer()
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        var nativeDir = _options.ResolveNativeDir();
        SherpaNative.Load(nativeDir);
        Version = Marshal.PtrToStringUTF8(SherpaNative.SherpaOnnxGetVersionStr()) ?? "?";

        var encoder = SherpaNativeOptions.ResolveAsset(_options.Encoder);
        var decoder = SherpaNativeOptions.ResolveAsset(_options.Decoder);
        var joiner = SherpaNativeOptions.ResolveAsset(_options.Joiner);
        var tokens = SherpaNativeOptions.ResolveAsset(_options.Tokens);
        foreach (var (name, path) in new[] { ("encoder", encoder), ("decoder", decoder), ("joiner", joiner), ("tokens", tokens) })
        {
            if (!File.Exists(path)) throw new FileNotFoundException($"ASR 模型文件缺失（{name}）: {path}", path);
        }

        // 建模单元与词表一致性（对齐安卓 vta_config：默认 cjkchar，逐字查表安全）：
        // 本仓库模型（multi-zh-hans zipformer，BBPE 2000 词表）**发布包不含 bpe.model**，
        // 若配 bbpe 而 bpe_vocab 为空，sherpa 的 EncodeHotwords 会用空编码器 → 原生崩溃。
        // 这里提前拦截成可读错误，而不是让进程 SIGSEGV；拿到配套 bpe.model 时
        // 设 ModelingUnit="bbpe" + BpeVocab=<bpe.model 路径> 即可让热词全量生效。
        var modelingUnit = _options.ModelingUnit.Trim();
        string bpeVocab = "";
        if (modelingUnit.Contains("bpe", StringComparison.OrdinalIgnoreCase))
        {
            if (string.IsNullOrWhiteSpace(_options.BpeVocab))
                throw new InvalidOperationException(
                    $"ModelingUnit=\"{modelingUnit}\" 需要配套 bpe.model（SherpaServer:BpeVocab），本仓库未随包提供；" +
                    "热词可用性优先，请保持 ModelingUnit=cjkchar，或补上 bpe.model 后重试");
            bpeVocab = SherpaNativeOptions.ResolveAsset(_options.BpeVocab);
            if (!File.Exists(bpeVocab))
                throw new FileNotFoundException($"BpeVocab 文件不存在: {bpeVocab}（ModelingUnit=\"{modelingUnit}\"）", bpeVocab);
        }

        using var strings = new Utf8Scope();
        var config = new SherpaNative.OnlineRecognizerConfig
        {
            FeatConfig = new SherpaNative.FeatureConfig { SampleRate = _options.SampleRate, FeatureDim = 80 },
            ModelConfig = new SherpaNative.OnlineModelConfig
            {
                Transducer = new SherpaNative.OnlineTransducerModelConfig
                {
                    Encoder = strings.Utf8(encoder),
                    Decoder = strings.Utf8(decoder),
                    Joiner = strings.Utf8(joiner),
                },
                Tokens = strings.Utf8(tokens),
                NumThreads = _options.NumThreads,
                Provider = strings.Utf8(_options.Provider),
                Debug = 0,
                ModelingUnit = strings.Utf8(modelingUnit),
                BpeVocab = strings.Utf8(bpeVocab),
            },
            DecodingMethod = strings.Utf8(_options.DecodingMethod),
            MaxActivePaths = _options.MaxActivePaths,
            EnableEndpoint = _options.EnableEndpoint ? 1 : 0,
            Rule1MinTrailingSilence = (float)_options.Rule1TrailingSilence,
            Rule2MinTrailingSilence = (float)_options.Rule2TrailingSilence,
            Rule3MinUtteranceLength = (float)_options.Rule3TrailingSilence,
            // 空串（非 NULL）：sherpa 热词编码在 NULL 下会崩；热词改为按流传入，此处不再读文件
            HotwordsFile = strings.Utf8(""),
            HotwordsScore = (float)_options.HotwordsScore,
        };

        var recognizer = SherpaNative.SherpaOnnxCreateOnlineRecognizer(ref config);
        if (recognizer == IntPtr.Zero)
            throw new InvalidOperationException(
                $"sherpa OnlineRecognizer 创建失败（版本 {Version}）：请检查模型路径、ModelingUnit={_options.ModelingUnit} 与原生库是否匹配");

        lock (_gate) _recognizer = recognizer;
        _logger.LogInformation(
            "[ASR] sherpa-onnx {Version} 进程内识别器就绪 {Ms}ms（{Dir}，threads={Threads}，modeling_unit={Unit}，热词按流传入）",
            Version, sw.ElapsedMilliseconds, nativeDir, _options.NumThreads, _options.ModelingUnit);
    }

    // ---- IHostedService：常驻加载（后台，不阻塞服务启动）----

    public Task StartAsync(CancellationToken cancellationToken)
    {
        _ = Task.Run(async () =>
        {
            try
            {
                await EnsureReadyAsync().ConfigureAwait(false);
                Console.WriteLine($"[MODELS] sherpa-onnx {Version} 已常驻加载（进程内，热词按表随流传入）");
            }
            catch (Exception ex)
            {
                _error = ex.Message;
                _logger.LogWarning(ex, "[MODELS] sherpa-onnx 常驻加载失败（语音暂不可用；首次语音使用会自动重试加载）");
            }
        });
        return Task.CompletedTask;
    }

    public Task StopAsync(CancellationToken cancellationToken)
    {
        Dispose();
        return Task.CompletedTask;
    }

    public void Dispose()
    {
        IntPtr recognizer;
        lock (_gate)
        {
            recognizer = _recognizer;
            _recognizer = IntPtr.Zero;
        }
        _ready = false;
        if (recognizer != IntPtr.Zero)
        {
            try { SherpaNative.SherpaOnnxDestroyOnlineRecognizer(recognizer); }
            catch (Exception ex) { _logger.LogWarning(ex, "[ASR] 释放识别器失败"); }
        }
        // 故意不 Dispose _loadLock：关闭阶段可能仍有请求在 EnsureReadyAsync 中等待，
        // 释放信号量会抛 ObjectDisposedException（进程即将退出，交给 GC 更安全）。
    }
}

/// <summary>
/// 单个连接的流式解码状态。热词随流传入（'/' 分隔多短语，短语内逐字空格分隔）；
/// 端点触发时由调用方取文本并 <see cref="Reset"/> 复用该流。
/// </summary>
internal sealed class SherpaStream : IDisposable
{
    private readonly IntPtr _recognizer;
    private readonly int _sampleRate;
    private readonly IntPtr _stream;
    private readonly IntPtr _hotwords;   // 保活本次热词串（C API 会拷贝，保活零成本、杜绝实现差异）
    private bool _disposed;

    internal SherpaStream(IntPtr recognizer, int sampleRate, string? hotwords)
    {
        _recognizer = recognizer;
        _sampleRate = sampleRate;
        _hotwords = string.IsNullOrEmpty(hotwords) ? IntPtr.Zero : Marshal.StringToCoTaskMemUTF8(hotwords);
        _stream = _hotwords == IntPtr.Zero
            ? SherpaNative.SherpaOnnxCreateOnlineStream(recognizer)
            : SherpaNative.SherpaOnnxCreateOnlineStreamWithHotwords(recognizer, _hotwords);
        if (_stream == IntPtr.Zero) throw new InvalidOperationException("sherpa CreateStream 失败");
    }

    /// <summary>喂入 float32 PCM（[-1,1]，本识别器采样率）。空块忽略（对齐安卓 PcmPipe 语义）。</summary>
    public void Accept(float[] samples)
    {
        if (_disposed || samples.Length == 0) return;
        SherpaNative.SherpaOnnxOnlineStreamAcceptWaveform(_stream, _sampleRate, samples, samples.Length);
    }

    /// <summary>排空解码（ready 循环）并返回当前识别文本。</summary>
    public string PumpDecode()
    {
        if (_disposed) return "";
        while (SherpaNative.SherpaOnnxIsOnlineStreamReady(_recognizer, _stream) != 0)
            SherpaNative.SherpaOnnxDecodeOnlineStream(_recognizer, _stream);
        return SherpaNative.ReadResultText(_recognizer, _stream);
    }

    /// <summary>当前文本（不解码）。</summary>
    public string Text() => _disposed ? "" : SherpaNative.ReadResultText(_recognizer, _stream);

    /// <summary>是否检测到端点（一句说完了）。</summary>
    public bool IsEndpoint() => !_disposed && SherpaNative.SherpaOnnxOnlineStreamIsEndpoint(_recognizer, _stream) != 0;

    /// <summary>收尾：告知输入结束（之后不得再喂音频）。</summary>
    public void InputFinished()
    {
        if (_disposed) return;
        SherpaNative.SherpaOnnxOnlineStreamInputFinished(_stream);
    }

    /// <summary>端点后复位解码状态，流可继续用于下一句（热词不变）。</summary>
    public void Reset()
    {
        if (_disposed) return;
        SherpaNative.SherpaOnnxOnlineStreamReset(_recognizer, _stream);
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        SherpaNative.SherpaOnnxDestroyOnlineStream(_stream);
        if (_hotwords != IntPtr.Zero) Marshal.FreeCoTaskMem(_hotwords);
    }
}

/// <summary>原生调用期间的 UTF-8 字符串作用域（调用返回即释放；sherpa 在调用内拷贝到 std::string）。</summary>
internal sealed class Utf8Scope : IDisposable
{
    private readonly List<IntPtr> _allocated = new();

    public IntPtr Utf8(string? value)
    {
        var ptr = Marshal.StringToCoTaskMemUTF8(value ?? "");
        _allocated.Add(ptr);
        return ptr;
    }

    public void Dispose()
    {
        foreach (var ptr in _allocated) Marshal.FreeCoTaskMem(ptr);
        _allocated.Clear();
    }
}

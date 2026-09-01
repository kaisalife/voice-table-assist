using System.Diagnostics;
using System.Net.WebSockets;
using System.Runtime.InteropServices;
using System.Text;

namespace VoiceTableAssist.Asr;

/// <summary>
/// 管理 sherpa-onnx 流式识别服务进程的生命周期（启动、健康检查、停止）。
/// 语音模型常驻：随服务启动后台拉起（加载模型需十几秒，hosted 启动不阻塞、服务秒级就绪），
/// 不随语义引擎的空闲卸载而停止；进程意外退出后由语音路径经 EnsureStartedAsync 自动重新拉起。
/// 服务退出时兜底清理。
/// </summary>
internal sealed class SherpaServerManager : IHostedService, IDisposable
{
    private readonly SherpaServerOptions _options;
    private readonly IConfiguration _configuration;
    private readonly ILogger<SherpaServerManager> _logger;
    private Process? _process;
    private CancellationTokenSource? _cts;
    private readonly SemaphoreSlim _startLock = new(1, 1);
    private TaskCompletionSource<bool>? _readyTcs;

    /// <summary>sherpa-onnx stderr 中需要抑制的刷屏模式（热词 token 不在 vocab 内，非致命）。</summary>
    private static readonly string[] _suppressedStderrPatterns =
        ["Cannot find ID for token"];

    public SherpaServerManager(IConfiguration configuration, ILogger<SherpaServerManager> logger)
    {
        _options = SherpaServerOptions.From(configuration);
        _configuration = configuration;
        _logger = logger;
    }

    /// <summary>子进程是否存活。</summary>
    public bool IsRunning => _process is { HasExited: false };

    /// <summary>进程活着且已完成就绪握手（stderr/stdout 出现 "Listening on:"）。</summary>
    public bool IsReady => _readyTcs is { } t && t.Task.IsCompletedSuccessfully;

    /// <summary>
    /// 确保子进程已启动并就绪（阻塞到 ready）；并发调用单飞。
    /// 快路径（已就绪）无锁直返；已在启动中则搭车等待同一次启动的就绪信号。
    /// </summary>
    public async Task EnsureStartedAsync()
    {
        if (IsRunning && IsReady) return;
        await _startLock.WaitAsync().ConfigureAwait(false);
        try
        {
            if (IsRunning && IsReady) return;
            await StartCoreAsync().ConfigureAwait(false);
        }
        finally { _startLock.Release(); }
    }

    /// <summary>
    /// 重启 sherpa 进程：sherpa 仅在启动时读取热词文件，导入/更新表后调用，
    /// 让解码偏置加载聚合后的最新热词。重启期间语音短暂不可用，会话会自动重试拉起。
    /// </summary>
    public async Task RestartAsync()
    {
        await _startLock.WaitAsync().ConfigureAwait(false);
        try
        {
            if (_process != null)
            {
                _logger.LogInformation("正在重启 sherpa-onnx（加载聚合后的最新热词）...");
                _readyTcs = null;
                try { _process.Kill(entireProcessTree: true); } catch { /* 进程可能已退出 */ }
                try { await _process.WaitForExitAsync().ConfigureAwait(false); } catch { }
                _process.Dispose();
                _process = null;
            }
            await StartCoreAsync().ConfigureAwait(false);
        }
        finally { _startLock.Release(); }
    }

    public Task StartAsync(CancellationToken cancellationToken)
    {
        // 常驻：后台拉起，不阻塞服务启动（Windows 服务模式 StartAsync 必须秒回，加载模型的十几秒不能占在这）。
        _ = Task.Run(async () =>
        {
            try
            {
                await EnsureStartedAsync().ConfigureAwait(false);
                _logger.LogInformation("[MODELS] sherpa-onnx 已常驻启动（后台）");
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex, "[MODELS] sherpa-onnx 常驻启动失败（语音暂不可用；首次语音使用会自动重试拉起）");
            }
        });
        return Task.CompletedTask;
    }

    /// <summary>
    /// 确保进程启动并就绪（调用方须持有 _startLock）：
    /// 未启动 → 启动进程并等就绪；已在启动中（后台那次）→ 搭车等就绪。
    /// </summary>
    private async Task StartCoreAsync()
    {
        if (IsRunning && IsReady) return;
        if (IsRunning)
        {
            await WaitReadyAsync().ConfigureAwait(false);
            return;
        }

        _logger.LogInformation("正在启动 sherpa-onnx 流式识别服务...");

        var exePath = ResolveSherpaAsset(_options.ExePath);
        var args = BuildArguments();

        _logger.LogInformation("sherpa-onnx 可执行文件: {Exe}", exePath);
        _logger.LogInformation("sherpa-onnx 启动参数: {Args}", args);

        if (!File.Exists(exePath))
        {
            _logger.LogWarning("sherpa-onnx 可执行文件不存在: {Exe}，将使用外部已启动的服务", exePath);
            return;
        }

        // 部署自愈：sherpa 启动时必须能读到热词文件（缺失会直接退出）。
        // 干净部署包里没有该文件（表尚未导入过），这里自动创建空文件占位，导入表后自动聚合填充。
        if (!string.IsNullOrWhiteSpace(_options.HotwordsFile))
        {
            var hotwordsPath = ResolvePath(_options.HotwordsFile);
            if (!File.Exists(hotwordsPath))
            {
                Directory.CreateDirectory(Path.GetDirectoryName(hotwordsPath)!);
                System.IO.File.WriteAllText(hotwordsPath, "", new UTF8Encoding(false));
                _logger.LogInformation("热词文件不存在，已自动创建空文件: {Path}", hotwordsPath);
            }
        }

        var startInfo = new ProcessStartInfo
        {
            FileName = exePath,
            Arguments = args,
            UseShellExecute = false,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            CreateNoWindow = true,
            WorkingDirectory = Path.GetDirectoryName(exePath)!
        };

        _readyTcs = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        _process = new Process { StartInfo = startInfo };

        _process.OutputDataReceived += (_, e) =>
        {
            if (string.IsNullOrEmpty(e.Data)) return;
            SignalReadyIfListening(e.Data);
            _logger.LogInformation("[sherpa-onnx] {Data}", e.Data);
        };
        _process.ErrorDataReceived += (_, e) =>
        {
            if (string.IsNullOrEmpty(e.Data)) return;
            SignalReadyIfListening(e.Data);
            // 抑制 sherpa-onnx 热词匹配中 token 不在 vocab 内的警告刷屏（非致命，热词会被跳过）
            foreach (var pat in _suppressedStderrPatterns)
                if (e.Data.Contains(pat, StringComparison.OrdinalIgnoreCase))
                    return;
            _logger.LogWarning("[sherpa-onnx-err] {Data}", e.Data);
        };

        _process.Start();
        _process.BeginOutputReadLine();
        _process.BeginErrorReadLine();
        _cts = new CancellationTokenSource();

        // 不吞异常：启动失败（进程退出/超时）向上传播——后台拉起方打 warning，语音路径让连接方拿到明确错误。
        await WaitReadyAsync().ConfigureAwait(false);
        _logger.LogInformation("sherpa-onnx 服务已就绪 (端口 {Port})", _options.Port);
    }

    public Task StopAsync(CancellationToken cancellationToken)
    {
        _cts?.Cancel();

        if (_process is { HasExited: false })
        {
            _logger.LogInformation("正在停止 sherpa-onnx 服务...");

            // 尝试优雅关闭
            if (RuntimeInformation.IsOSPlatform(OSPlatform.Windows))
            {
                _process.CloseMainWindow();
            }
            else
            {
                _process.Kill();
            }

            if (!_process.WaitForExit(5000))
            {
                _logger.LogWarning("sherpa-onnx 未在 5 秒内退出，强制终止");
                _process.Kill();
            }

            _logger.LogInformation("sherpa-onnx 服务已停止");
        }

        return Task.CompletedTask;
    }

    public void Dispose()
    {
        _cts?.Dispose();
        _process?.Dispose();
    }

    /// <summary>
    /// 等待就绪信号（_readyTcs 完成）。不再用探测连接测端口：探测连接会硬断开，可能打乱
    /// sherpa 的 websocket 接收循环，导致紧随其后的真实识别连接挂起或把服务端进程弄死。
    /// </summary>
    private async Task WaitReadyAsync()
    {
        var ready = _readyTcs ?? throw new InvalidOperationException("sherpa-onnx 就绪信号未初始化");
        var process = _process ?? throw new InvalidOperationException("sherpa-onnx 进程未启动");

        using var linked = CancellationTokenSource.CreateLinkedTokenSource(_cts?.Token ?? CancellationToken.None);
        linked.CancelAfter(TimeSpan.FromSeconds(_options.StartupTimeoutSeconds));

        // 进程异常退出（模型路径错误、端口被占用等）要立刻失败，而不是拖到超时
        var exited = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
        void OnExited(object? s, EventArgs e) => exited.TrySetResult(true);
        process.EnableRaisingEvents = true;
        process.Exited += OnExited;
        if (process.HasExited) exited.TrySetResult(true);
        try
        {
            var winner = await Task.WhenAny(
                ready.Task,
                exited.Task,
                Task.Delay(Timeout.InfiniteTimeSpan, linked.Token)).ConfigureAwait(false);

            if (winner == exited.Task)
                throw new InvalidOperationException("sherpa-onnx 进程意外退出，请检查模型文件路径和端口是否被占用");
            if (winner != ready.Task)
                throw new OperationCanceledException("sherpa-onnx 启动超时");

            await ready.Task.ConfigureAwait(false); // 已完成，传播潜在异常
        }
        finally { process.Exited -= OnExited; }
    }

    /// <summary>sherpa 就绪信号：stderr/stdout 打出 "Listening on:" 即代表端口已监听。</summary>
    private void SignalReadyIfListening(string data)
    {
        if (data.Contains("Listening on:", StringComparison.OrdinalIgnoreCase))
            _readyTcs?.TrySetResult(true);
    }

    private string BuildArguments()
    {
        var args = new List<string>
        {
            $"--encoder={QuotePath(ResolveSherpaAsset(_options.Encoder))}",
            $"--decoder={QuotePath(ResolveSherpaAsset(_options.Decoder))}",
            $"--joiner={QuotePath(ResolveSherpaAsset(_options.Joiner))}",
            $"--tokens={QuotePath(ResolveSherpaAsset(_options.Tokens))}",
            $"--port={_options.Port}",
            $"--num-threads={_options.NumThreads}",
            $"--decoding-method={_options.DecodingMethod}",
        };

        if (!string.IsNullOrWhiteSpace(_options.HotwordsFile))
        {
            args.Add($"--hotwords-file={QuotePath(ResolvePath(_options.HotwordsFile))}");
            args.Add($"--hotwords-score={_options.HotwordsScore}");
        }

        args.Add($"--enable-endpoint={(_options.EnableEndpoint ? "true" : "false")}");
        if (_options.EnableEndpoint)
        {
            // rule1：停顿 Rule1TrailingSilence 秒即切句输出 final；rule2/3 置超大值禁用
            args.Add($"--rule1-min-trailing-silence={_options.Rule1TrailingSilence}");
            args.Add($"--rule2-min-trailing-silence={_options.Rule2TrailingSilence}");
            args.Add($"--rule3-min-trailing-silence={_options.Rule3TrailingSilence}");
        }

        return string.Join(" ", args);
    }

    /// <summary>模型根目录：RANER_MODEL_DIR 优先，否则 exe 目录下的 models/（与 ModelBootstrap 解析一致）。</summary>
    private static string GetModelRoot()
    {
        var envModels = Environment.GetEnvironmentVariable("RANER_MODEL_DIR");
        return string.IsNullOrWhiteSpace(envModels)
            ? Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "models"))
            : Path.GetFullPath(envModels);
    }

    /// <summary>
    /// 解析 sherpa 资产（exe / 模型文件）：先按 exe 目录相对解析；不存在则回退模型根
    /// （开发机 RANER_MODEL_DIR 指向源码 models，运行时归档 models/sherpa-onnx、ASR 模型 models/asr 都在模型根下）。
    /// </summary>
    private static string ResolveSherpaAsset(string rel)
    {
        var p = ResolvePath(rel);
        if (File.Exists(p)) return p;

        var relUnderRoot = rel.StartsWith("models/", StringComparison.OrdinalIgnoreCase)
            ? rel["models/".Length..]
            : rel;
        var fallback = Path.GetFullPath(Path.Combine(GetModelRoot(), relUnderRoot));
        return File.Exists(fallback) ? fallback : p;
    }

    /// <summary>将相对路径转换为基于可执行文件所在目录的绝对路径。</summary>
    private static string ResolvePath(string path)
    {
        if (Path.IsPathRooted(path)) return path;
        return Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, path));
    }

    private static string QuotePath(string path) => $"\"{path}\"";
}

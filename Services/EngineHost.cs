using System.Diagnostics;
using VoiceTableAssist.Asr;

namespace VoiceTableAssist.Services;

/// <summary>
/// 引擎宿主：RaNER + gte-base-zh 语义引擎按需加载、空闲卸载（默认 30s，Models:IdleUnloadSeconds 可配）。
/// 语音模型（sherpa-onnx）常驻：随服务启动后台拉起、不随空闲卸载停止（由 SherpaServerManager 托管）。
/// 空闲判定：距最后一次使用（HTTP 查询 / WS 语音活动）超过阈值，且无活跃语音会话。
/// 卸载动作：dispose 两个 ONNX 语义引擎（sherpa 子进程不受影响）。
/// </summary>
internal sealed class EngineHost : IHostedService, IDisposable
{
    private readonly IServiceProvider _services;
    private readonly ModelPaths _paths;
    private readonly IConfiguration _config;
    private readonly ILogger<EngineHost> _logger;
    private readonly object _gate = new();

    private RaNerEngine? _raner;
    private EmbeddingEngine? _embed;
    private Task? _loading;
    private volatile bool _loaded;
    private DateTime _lastTouchUtc;
    private int _activeSessions;
    private PeriodicTimer? _idleTimer;
    private CancellationTokenSource? _idleCts;

    public EngineHost(IServiceProvider services, ModelPaths paths, IConfiguration config, ILogger<EngineHost> logger)
    {
        _services = services;
        _paths = paths;
        _config = config;
        _logger = logger;
    }

    /// <summary>语义引擎懒加载（默认 true）：启动不加载 RaNER/嵌入，首次使用才加载；false=启动即加载（旧行为）。语音模型不受此开关影响（始终常驻）。</summary>
    public bool LazyLoad => _config.GetValue("Models:LazyLoad", true);

    /// <summary>空闲卸载阈值（秒），默认 30。</summary>
    public int IdleUnloadSeconds => _config.GetValue("Models:IdleUnloadSeconds", 30);

    public bool IsLoaded => _loaded;

    /// <summary>已加载的 RaNER 引擎；未加载时抛异常（调用方应先 EnsureEnginesAsync）。</summary>
    public RaNerEngine Raner => _raner ?? throw new InvalidOperationException("RaNER 引擎未加载（应先调用 EnsureEnginesAsync）");

    /// <summary>已加载的嵌入引擎；未加载时抛异常。</summary>
    public EmbeddingEngine Embed => _embed ?? throw new InvalidOperationException("嵌入引擎未加载（应先调用 EnsureEnginesAsync）");

    /// <summary>
    /// 确保语义引擎（RaNER + gte）已加载。单飞（single-flight）：并发调用共享同一次加载。
    /// progress 回调用于向等待方推送加载进度（如 WS 连接的 loading 帧）。
    /// </summary>
    public Task EnsureEnginesAsync(Action<string>? progress = null)
    {
        lock (_gate)
        {
            if (_loaded) { Touch(); return Task.CompletedTask; }
            if (_loading is not null) return _loading;   // 已有在途加载，搭车等待
            _loading = LoadEnginesAsync(progress);
            return _loading;
        }
    }

    /// <summary>确保 sherpa-onnx 子进程已启动并就绪（语音模型常驻托管；进程意外退出后由此兜底重启）。</summary>
    public async Task EnsureSherpaAsync(Action<string>? progress = null)
    {
        var sherpa = _services.GetService<SherpaServerManager>();
        if (sherpa is null) return;
        if (sherpa.IsRunning && sherpa.IsReady) { Touch(); return; }
        progress?.Invoke("正在启动语音识别引擎（首次使用需数秒）...");
        await sherpa.EnsureStartedAsync().ConfigureAwait(false);
        Touch();
    }

    /// <summary>标记刚被使用（推迟空闲卸载）。WS 会话的识别活动、HTTP 查询都会调用。</summary>
    public void Touch() => _lastTouchUtc = DateTime.UtcNow;

    /// <summary>
    /// 抢占唯一语音会话位：同时只允许一个语音连接存活（单连接门卫）。
    /// 成功返回 true 并 Touch() 推迟空闲卸载；失败返回 false，调用方应拒绝该连接（如 HTTP 409）。
    /// </summary>
    public bool TryAcquireSession()
    {
        if (Interlocked.CompareExchange(ref _activeSessions, 1, 0) != 0) return false;
        Touch();
        return true;
    }

    /// <summary>释放语音会话位（会话结束调用，须与 TryAcquireSession 配对）。</summary>
    public void ReleaseSession() => Interlocked.Exchange(ref _activeSessions, 0);

    // ---- IHostedService ----

    public Task StartAsync(CancellationToken cancellationToken)
    {
        _idleCts = new CancellationTokenSource();
        _idleTimer = new PeriodicTimer(TimeSpan.FromSeconds(5));
        _ = Task.Run(() => IdleLoopAsync(_idleCts.Token));
        // 语音模型（sherpa）常驻：由 SherpaServerManager 的 hosted 启动后台拉起，这里只管语义引擎的加载时机。
        if (LazyLoad)
        {
            _lastTouchUtc = DateTime.UtcNow;
            Console.WriteLine($"[MODELS] 语义引擎（RaNER/嵌入）懒加载：首次使用约 3~8s，空闲 {IdleUnloadSeconds}s 自动卸载；语音模型常驻（后台启动）");
            return Task.CompletedTask;
        }
        Console.WriteLine("[MODELS] 语义引擎启动即加载（Models:LazyLoad=false）；语音模型常驻（后台启动）");
        return EnsureEnginesAsync(null);
    }

    public Task StopAsync(CancellationToken cancellationToken)
    {
        _idleCts?.Cancel();
        _idleTimer?.Dispose();
        Unload();   // 进程退出时释放引擎（sherpa 由 hosted service 停止）
        return Task.CompletedTask;
    }

    public void Dispose()
    {
        _idleCts?.Cancel();
        _idleCts?.Dispose();
        _idleTimer?.Dispose();
        Unload();   // 退出时兜底释放
    }

    // ---- 私有实现 ----

    private async Task IdleLoopAsync(CancellationToken cancellationToken)
    {
        try
        {
            while (await _idleTimer!.WaitForNextTickAsync(cancellationToken).ConfigureAwait(false))
            {
                if (!_loaded) continue;
                if (Volatile.Read(ref _activeSessions) > 0) continue;
                if (DateTime.UtcNow - _lastTouchUtc <= TimeSpan.FromSeconds(IdleUnloadSeconds)) continue;
                Unload();
            }
        }
        catch (OperationCanceledException) { }
        catch (Exception ex) { _logger.LogWarning(ex, "[MODELS] 空闲巡检异常"); }
    }

    private async Task LoadEnginesAsync(Action<string>? progress)
    {
        try
        {
            var sw = Stopwatch.StartNew();
            progress?.Invoke("正在加载语义解析模型（首次使用需数秒）...");
            var raner = await Task.Run(() => new RaNerEngine(_paths.RanerDir)).ConfigureAwait(false);
            var embed = await Task.Run(() =>
            {
                var eng = new EmbeddingEngine(_paths.EmbedDir);
                // 检索低置信过滤：余弦低于阈值的最近邻视为未命中（宁缺勿错填）；0=不过滤
                eng.MinSim = _config.GetValue("Embedding:MinSim", 0.55f);
                return eng;
            }).ConfigureAwait(false);
            lock (_gate)
            {
                _raner = raner;
                _embed = embed;
                _loaded = true;
                _lastTouchUtc = DateTime.UtcNow;
            }
            _logger.LogInformation("[MODELS] 引擎按需加载完成 {Ms}ms (RaNER {Labels}类标签)", sw.ElapsedMilliseconds, RaNerEngine.LABELS.Length);
            progress?.Invoke("模型加载完成");
        }
        catch (Exception ex)
        {
            _logger.LogError(ex, "[MODELS] 引擎加载失败");
            throw;
        }
        finally
        {
            lock (_gate) _loading = null;   // 成功后走 _loaded 快路径；失败后允许重试
        }
    }

    /// <summary>卸载：dispose 语义引擎（RaNER/嵌入）+ 强制回收。语音模型（sherpa）常驻，不随空闲卸载停止。</summary>
    private void Unload()
    {
        RaNerEngine? raner; EmbeddingEngine? embed;
        lock (_gate)
        {
            if (!_loaded) return;
            _loaded = false;
            raner = _raner; embed = _embed;
            _raner = null; _embed = null;
        }
        var before = Environment.WorkingSet;
        try { raner?.Dispose(); } catch (Exception ex) { _logger.LogWarning(ex, "[MODELS] RaNER 释放失败"); }
        try { embed?.Dispose(); } catch (Exception ex) { _logger.LogWarning(ex, "[MODELS] 嵌入引擎释放失败"); }

        GC.Collect();
        GC.WaitForPendingFinalizers();
        GC.Collect();
        _logger.LogInformation("[MODELS] 空闲 {Sec}s，已卸载语义引擎（语音模型常驻不卸载）：工作集 {BeforeMB}MB -> {AfterMB}MB",
            IdleUnloadSeconds, before / 1024 / 1024, Environment.WorkingSet / 1024 / 1024);
    }
}

using VoiceTableAssist.Services;

namespace VoiceTableAssist.Services;

/// <summary>
/// 空闲卸载调度：以 PeriodicTimer 按 Tables:IdleCheckInterval（默认 1min）周期调用
/// TableVectorManager.UnloadIfIdle()，超过 IdleTimeout（默认 30min）未活动的表自动卸载（仅内存）。
/// </summary>
internal sealed class TableIdleMonitor : IHostedService, IDisposable
{
    private readonly TableVectorManager _manager;
    private readonly IConfiguration _config;
    private readonly ILogger<TableIdleMonitor> _logger;
    private Timer? _timer;

    public TableIdleMonitor(TableVectorManager manager, IConfiguration config, ILogger<TableIdleMonitor> logger)
    {
        _manager = manager;
        _config = config;
        _logger = logger;
    }

    public Task StartAsync(CancellationToken cancellationToken)
    {
        var interval = _config.GetValue("Tables:IdleCheckInterval", TimeSpan.FromMinutes(1));
        _timer = new Timer(_ => Tick(), null, interval, interval);
        _logger.LogInformation("[TABLES] 空闲卸载监控启动，检查间隔 {Interval}", interval);
        return Task.CompletedTask;
    }

    public Task StopAsync(CancellationToken cancellationToken)
    {
        _timer?.Change(Timeout.Infinite, Timeout.Infinite);
        return Task.CompletedTask;
    }

    private void Tick()
    {
        try { _manager.UnloadIfIdle(); }
        catch (Exception ex) { _logger.LogError(ex, "[TABLES] 空闲卸载巡检异常"); }
    }

    public void Dispose() => _timer?.Dispose();
}
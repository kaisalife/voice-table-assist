using System.Net.WebSockets;
using VoiceTableAssist.Dtos;
using VoiceTableAssist.Infrastructure;
using VoiceTableAssist.Services;

namespace VoiceTableAssist.Asr;

/// <summary>
/// 每条 WS 连接一个的服务端交互编排器：
/// 累积流式 final 文本 → 静默超时自动提交 → 进程内 RaNER + 向量检索 →
/// 直接下发解析好的单元格（type=cells）。前端无需再做文本合并/静默判定/二次 HTTP 调用。
/// 提交后累计文本保留不清（后续语句常省略行标签，需前文主体），仅超 MaxChars 时整体清空重录。
/// </summary>
internal sealed class VoiceInteractionSession : IDisposable
{
    private readonly TimeSpan _silence;
    private readonly int _maxChars;
    private readonly EngineHost _host;
    private readonly TableVectorManager _manager;   // 单连接：活动表即本会话表，提交时直接查当前活动索引
    private readonly ConnectionSender _sender;
    private readonly ILogger _logger;
    private readonly object _gate = new();
    private readonly string _tableKey;
    private string _accumulated = "";
    private CancellationTokenSource? _timerCts;

    public VoiceInteractionSession(
        EngineHost host, TableVectorManager manager, TimeSpan silence,
        ConnectionSender sender, ILogger logger, int maxChars,
        string tableKey)
    {
        _host = host;
        _manager = manager;
        _silence = silence;
        _sender = sender;
        _logger = logger;
        _maxChars = maxChars;
        _tableKey = tableKey;
    }

    /// <summary>当前累计文本（线程安全快照；提交/溢出清空后为空串）。</summary>
    public string Accumulated { get { lock (_gate) return _accumulated; } }

    /// <summary>
    /// 由桥的下行循环在收到 final 识别片段时调用；自动重启静默计时。
    /// 返回合并后的最新累计文本（溢出清空返回空串），随事件下发供前端调试显示。
    /// </summary>
    public string OnFinal(string text)
    {
        text = text.Trim();
        if (text.Length == 0) return Accumulated;
        _host.Touch();   // 语音活动推迟空闲卸载

        bool overflow = false;
        string merged;
        lock (_gate)
        {
            merged = MergeText(_accumulated, text);
            if (merged.Length > _maxChars)
            {
                // 累积过多（如用户一直没停顿）：丢弃本次累积，防止把无关内容一并解析
                _accumulated = "";
                overflow = true;
            }
            else
            {
                _accumulated = merged;
            }
        }

        if (overflow)
        {
            CancelTimer();
            var message = $"语音累计超过 {_maxChars} 字，已自动清空，请分次录入";
            _logger.LogInformation("累积溢出清空: 上限={MaxChars}", _maxChars);
            _ = Task.Run(async () =>
                await _sender.SendAsync(new BrowserEvent("error", Code: "ACCUM_OVERFLOW", Message: message), CancellationToken.None));
            return "";
        }

        RestartTimer();
        return merged;
    }

    /// <summary>立即提交累积文本（客户端发 {"type":"stop"} 或会话收尾时调用）。</summary>
    public Task FlushAsync(CancellationToken cancellationToken)
    {
        CancelTimer();
        return SubmitAsync(cancellationToken);
    }

    /// <summary>
    /// 把未进入累计的最新一段 partial 文本并入累计（不触发静默计时器、不触发超限清空）。
    /// 用于"点结束会话"时收尾：partial 只下发 UI、不进 _accumulated，直接 Flush 会因累计为空而漏发 cells。
    /// </summary>
    public void FoldPartial(string? text)
    {
        var trimmed = text?.Trim();
        if (string.IsNullOrEmpty(trimmed)) return;
        lock (_gate)
        {
            _accumulated = MergeText(_accumulated, trimmed);
        }
    }

    // ---- 私有实现 ----

    private void RestartTimer()
    {
        CancellationTokenSource cts;
        lock (_gate)
        {
            CancelTimer();
            cts = new CancellationTokenSource(_silence);
            _timerCts = cts;
        }
        var token = cts.Token;
        var mine = cts;
        _ = Task.Run(async () =>
        {
            try
            {
                await Task.Delay(_silence, token);
                if (_timerCts == mine) await SubmitAsync(CancellationToken.None);
            }
            catch (OperationCanceledException) { }
            catch (Exception ex) { _logger.LogWarning(ex, "静默提交失败"); }
            finally
            {
                mine.Dispose();
                if (_timerCts == mine) _timerCts = null;
            }
        }, CancellationToken.None);
    }

    private void CancelTimer()
    {
        lock (_gate)
        {
            _timerCts?.Cancel();
            _timerCts = null;   // 令回调里的引用比对失效，防止已取消的计划仍提交
        }
    }

    private async Task SubmitAsync(CancellationToken cancellationToken)
    {
        // 提交后保留累计文本（不清空）：后续语句常省略行标签（如「二号是六十」），
        // 需要依赖前文主体才能正确解析；重复的格子会以相同/更新的值再填一遍，无害。
        string text;
        lock (_gate) { text = _accumulated.Trim(); }
        if (text.Length == 0) return;

        try
        {
            // 单连接门卫保证整个会话期间全局活动表就是本会话绑定的表：
            // 握手已同步 Activate（唯一会话，活动表不会中途被别的连接切走），提交时直接查当前活动索引。
            _host.Touch();
            var idx = _manager.ActiveIndex;
            var bio = _host.Raner.Predict(text);
            var cells = new List<CellDto>();
            foreach (var (sub, obj, val) in TripleExtractor.Extract(bio))
            {
                var (row, col, _, _) = _host.Embed.Lookup(sub + obj, idx);
                if (row <= 0) continue;   // 向量检索低置信未命中，跳过该格
                cells.Add(new CellDto { row = row, column = col, values = ChineseNumeral.ToDecimal(val) });
            }
            await _sender.SendAsync(new BrowserEvent("cells", Text: text, IsFinal: true, Cells: cells), cancellationToken);
        }
        catch (OperationCanceledException) { throw; }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "语音交互解析失败: {Text}", text);
            await _sender.SendAsync(new BrowserEvent("error", Code: "PARSE_FAILED", Message: $"解析失败：{ex.Message}"), CancellationToken.None);
        }
    }

    /// <summary>与验证页一致的流式文本合并（前缀扩展 / 包含去重 / 重叠拼接）。</summary>
    private static string MergeText(string prev, string cur)
    {
        if (string.IsNullOrEmpty(prev)) return cur;
        if (cur.Length == 0 || prev.Contains(cur)) return prev;
        if (cur.Contains(prev)) return cur;

        var limit = Math.Min(prev.Length, cur.Length);
        for (var overlap = limit; overlap >= 2; overlap--)
        {
            if (prev[^overlap..] == cur[..overlap]) return prev + cur[overlap..];
        }
        return prev + " " + cur;
    }

    public void Dispose() => CancelTimer();
}

/// <summary>单连接发送串行化：WebSocket 同一时刻只允许一个未完成 Send，多来源并发发送必须排队。</summary>
internal sealed class ConnectionSender : IDisposable
{
    private readonly WebSocket _browser;
    private readonly SemaphoreSlim _lock = new(1, 1);

    public ConnectionSender(WebSocket browser) => _browser = browser;

    public bool IsOpen => _browser.State == WebSocketState.Open;

    public async Task SendAsync(BrowserEvent item, CancellationToken cancellationToken)
    {
        await _lock.WaitAsync(cancellationToken);
        try { await WsHelper.SendJsonAsync(_browser, item, cancellationToken); }
        finally { _lock.Release(); }
    }

    public void Dispose() => _lock.Dispose();

    /// <summary>按配置构造每连接会话的工厂（绑定连接的表；索引取全局活动表，由单连接门卫保证一致）。</summary>
    public static Func<ConnectionSender, VoiceInteractionSession> CreateFactory(
        IConfiguration configuration, IServiceProvider services, ILogger logger,
        string tableKey)
    {
        var ms = configuration["Interaction:SilenceMs"];
        if (!int.TryParse(ms, out var silenceMs) || silenceMs <= 0) silenceMs = 2500;
        var silence = TimeSpan.FromMilliseconds(silenceMs);

        var chars = configuration["Interaction:MaxChars"];
        if (!int.TryParse(chars, out var maxChars) || maxChars <= 0) maxChars = 120;

        return sender => new VoiceInteractionSession(
            services.GetRequiredService<EngineHost>(),
            services.GetRequiredService<TableVectorManager>(),
            silence, sender, logger, maxChars, tableKey);
    }
}

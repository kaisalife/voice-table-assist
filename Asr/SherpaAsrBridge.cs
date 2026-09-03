using System.Net.WebSockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using VoiceTableAssist.Infrastructure;
using VoiceTableAssist.Services;

namespace VoiceTableAssist.Asr;

/// <summary>
/// 连接本地 sherpa-onnx 流式识别服务（sherpa-onnx-online-websocket-server）。
/// 协议：二进制帧为 float32 采样（原生端序）上行，JSON 文本下行；
/// 下行消息含 text / is_final / is_eof，映射为 partial / final 回传浏览器。
/// 浏览器侧统一上行 float32 PCM，网关原样透传给 sherpa。
/// 可选挂接 VoiceInteractionSession：服务端静默自动提交并下发解析好的 cells。
/// 懒加载模式：连接期先经 EngineHost 按需装载引擎/拉起 sherpa，
/// 期间向浏览器下发 type=loading 进度帧（前端可展示"模型加载中"）。
/// </summary>
internal static class SherpaAsrBridge
{
    public static async Task RunAsync(
        HttpContext context, SherpaOptions options, HomophoneReplacer? replacer, CancellationToken aborted,
        CancellationToken takeover, GtcrnDenoiser? denoiser = null,
        Func<ConnectionSender, VoiceInteractionSession>? sessionFactory = null)
    {
        var logger = context.RequestServices.GetRequiredService<ILoggerFactory>().CreateLogger("AsrGateway");
        var host = context.RequestServices.GetRequiredService<EngineHost>();
        using var browser = await context.WebSockets.AcceptWebSocketAsync();
        using var sender = new ConnectionSender(browser);
        using var interaction = sessionFactory?.Invoke(sender);
        using var local = new ClientWebSocket();
        using var session = CancellationTokenSource.CreateLinkedTokenSource(aborted, takeover);

        try
        {
            // 懒加载：首次使用时在此装载引擎并拉起 sherpa（发 loading 进度帧给前端）。
            // 后续连接引擎常驻，此步毫秒级通过。
            await host.EnsureEnginesAsync(msg => SendLoading(sender, msg, session.Token));
            await host.EnsureSherpaAsync(msg => SendLoading(sender, msg, session.Token));

            // 上游连接必须有上限：sherpa 半死（listen 存在但不 accept）时会无限挂起，
            // 导致浏览器永远收不到 ready 也没有 error（表象就是前端"连接中"无限转圈）。
            var sw = System.Diagnostics.Stopwatch.StartNew();
            using var upstreamCts = CancellationTokenSource.CreateLinkedTokenSource(session.Token);
            upstreamCts.CancelAfter(TimeSpan.FromSeconds(30));
            try
            {
                await local.ConnectAsync(new Uri(options.Endpoint), upstreamCts.Token);
            }
            catch (OperationCanceledException) when (!session.Token.IsCancellationRequested)
            {
                throw new TimeoutException("ASR 后端 sherpa-onnx 连接超时(30s)，请检查 sherpa 进程是否存活");
            }
            logger.LogInformation("WS 会话：上游 sherpa 连接成功 {ElapsedMs}ms", sw.ElapsedMilliseconds);

            await sender.SendAsync(new BrowserEvent("ready"), session.Token);

            // 降噪器是进程级单例：每通会话开始必须复位（GRU/STFT/overlap-add 状态），
            // 否则上一通对话的尾帧会串进本通的开头几帧。
            denoiser?.Reset();

            // 最终 final 信号：stop 时先等识别收尾再提交，避免提交空文本、
            // 以及"静默计时器提交撞上连接已释放"的时序竞争（cells 永远发不出去）。
            var finalReceived = new TaskCompletionSource(TaskCreationOptions.RunContinuationsAsynchronously);
            var audioBox = new long[1];   // 会话上行音频字节诊断（Interlocked 累加）
            // 最近一条 partial 文本（single-element holder 跨方法共享）：stop 时把它补 fold 进累计，
            // 否则 sherpa 端点未切句就停止会导致累计为空、NER 漏掉最后一次识别。
            var lastPartial = new string?[1];

            var upload = ForwardBrowserAudioAsync(browser, local, interaction, finalReceived.Task, n => Interlocked.Add(ref audioBox[0], n), session.Token, lastPartial, denoiser);
            var download = ForwardRecognitionAsync(local, sender, interaction, replacer, finalReceived, session.Token, lastPartial);

            var first = await Task.WhenAny(upload, download);
            if (first == upload)
            {
                try { await download.WaitAsync(TimeSpan.FromSeconds(3)); }
                catch (TimeoutException) { }
            }
            session.Cancel();
            await WsHelper.IgnoreCancellationAsync(upload, download);
            // 诊断：区分"浏览器没发音频"与"发了但识别无输出"
            var bytes = Interlocked.Read(ref audioBox[0]);
            if (bytes > 0)
                logger.LogInformation("WS 会话结束：收到浏览器音频 {KB:F0} KB（约 {Sec:F1}s）", bytes / 1024.0, bytes / 4.0 / 16000);
            else
                logger.LogWarning("WS 会话结束：浏览器未上行任何音频数据（检查麦克风采集/AudioContext 是否 suspended）");
        }
        catch (OperationCanceledException) when (session.IsCancellationRequested) { }
        catch (Exception exception)
        {
            await WsHelper.TrySendErrorAsync(browser, "ASR_CONNECTION", exception.Message);
        }
        finally
        {
            await WsHelper.TryCloseAsync(local);
            await WsHelper.TryCloseAsync(browser);
        }
    }

    private static void SendLoading(ConnectionSender sender, string message, CancellationToken cancellationToken)
    {
        try { _ = sender.SendAsync(new BrowserEvent("loading", Message: message), cancellationToken); }
        catch (Exception) { /* 进度帧失败不影响主流程 */ }
    }

    private static async Task ForwardBrowserAudioAsync(
        WebSocket browser,
        WebSocket local,
        VoiceInteractionSession? interaction,
        Task finalReceived,
        Action<long> addAudioBytes,
        CancellationToken cancellationToken,
        string?[] lastPartial,
        GtcrnDenoiser? denoiser = null)
    {
        while (browser.State == WebSocketState.Open && local.State == WebSocketState.Open)
        {
            var message = await WsHelper.ReceiveCompleteMessageAsync(browser, cancellationToken);
            if (message is null) return;

            if (message.Value.Type == WebSocketMessageType.Text)
            {
                var command = Encoding.UTF8.GetString(message.Value.Payload);
                if (command.Contains("\"type\":\"stop\"", StringComparison.OrdinalIgnoreCase))
                {
                    // 先让 sherpa 输出最终 final（一般 <1s），再立即提交——
                    // 否则此刻 accumulated 为空，真正提交只能靠 2.5s 静默计时器，
                    // 而连接生命周期等不了那么久（cells 丢失）。
                    // 降噪器 flush：把 < hop 的尾帧补零收尾并复位状态（输出丢弃，仅为清态）。
                    try { denoiser?.Denoise(Array.Empty<float>(), flush: true); } catch { }
                    await WsHelper.SendTextAsync(local, "Done", cancellationToken);
                    try { await finalReceived.WaitAsync(TimeSpan.FromSeconds(5), cancellationToken); }
                    catch (TimeoutException) { }
                    // 把最近一条还没进累计的 partial 文本补 fold（partial 只下发 UI，不进 _accumulated）。
                    if (interaction is not null) interaction.FoldPartial(lastPartial[0]);
                    if (interaction is not null) await interaction.FlushAsync(cancellationToken);
                    return;
                }
                continue;
            }

            if (message.Value.Type != WebSocketMessageType.Binary) continue;

            // 浏览器上行 float32 PCM，网关原样透传给 sherpa；若启用了 GTCRN 降噪，
            // 则先解码成 float[] → Denoise() → 重新编码回 byte[] 再上行。
            var payload = message.Value.Payload;
            addAudioBytes(payload.Length);

            if (denoiser is not null)
            {
                var samples = BytesToFloats(payload);
                var denoised = denoiser.Denoise(samples);
                var outBytes = FloatsToBytes(denoised);
                await local.SendAsync(new ArraySegment<byte>(outBytes), WebSocketMessageType.Binary, true, cancellationToken);
            }
            else
            {
                await local.SendAsync(new ArraySegment<byte>(payload), WebSocketMessageType.Binary, true, cancellationToken);
            }
        }
    }

    /// <summary>IEEE-754 小端 float32 字节 → float[]。</summary>
    private static float[] BytesToFloats(ReadOnlyMemory<byte> bytes) =>
        MemoryMarshal.Cast<byte, float>(bytes.Span).ToArray();

    /// <summary>float[] → IEEE-754 小端 float32 字节。</summary>
    private static byte[] FloatsToBytes(float[] samples)
    {
        var bytes = new byte[samples.Length * 4];
        Buffer.BlockCopy(samples, 0, bytes, 0, bytes.Length);
        return bytes;
    }

    private static async Task ForwardRecognitionAsync(
        WebSocket local,
        ConnectionSender sender,
        VoiceInteractionSession? interaction,
        HomophoneReplacer? replacer,
        TaskCompletionSource finalReceived,
        CancellationToken cancellationToken,
        string?[] lastPartial)
    {
        while (local.State == WebSocketState.Open && sender.IsOpen)
        {
            var message = await WsHelper.ReceiveCompleteMessageAsync(local, cancellationToken);
            if (message is null) return;
            if (message.Value.Type != WebSocketMessageType.Text) continue;

            BrowserEvent? item;
            try { item = ParseSherpaMessage(Encoding.UTF8.GetString(message.Value.Payload)); }
            catch { continue; }

            if (item is not null)
            {
                if (item.Text is not null)
                {
                    // 同音/专名纠正（拼音=汉字），再执行数字/列号语境的上下文替换
                    var text = replacer?.Apply(item.Text) ?? item.Text;
                    item = item with { Text = DomainCorrection.Correct(text) };
                }

                // 服务端交互编排：final 片段进入静默自动提交流程。
                // 事件携带服务端当前累计文本（Accumulated），前端可直接展示调试，无需自行合并。
                if (interaction is not null)
                {
                    if (item.IsFinal == true && !string.IsNullOrWhiteSpace(item.Text))
                    {
                        item = item with { Accumulated = interaction.OnFinal(item.Text) };
                        finalReceived.TrySetResult();   // accumulated 已落定，stop 侧可以提交了
                        lastPartial[0] = null;          // final 覆盖了前面的 partial，无需再 fold
                    }
                    else
                    {
                        // 记录最近一条 partial 文本：stop 时（final 还没来）用它补 fold 进累计
                        if (!string.IsNullOrWhiteSpace(item.Text)) lastPartial[0] = item.Text;
                        item = item with { Accumulated = interaction.Accumulated };
                    }
                }

                await sender.SendAsync(item, cancellationToken);
            }
        }
    }

    /// <summary>把 sherpa-onnx 下行 JSON 映射为浏览器事件；无识别文本的消息返回 null。</summary>
    private static BrowserEvent? ParseSherpaMessage(string json)
    {
        using var document = JsonDocument.Parse(json);
        var root = document.RootElement;
        if (!root.TryGetProperty("text", out var textNode)) return null;
        var text = textNode.GetString() ?? "";
        if (string.IsNullOrEmpty(text)) return null;

        var isFinal = root.TryGetProperty("is_final", out var finalNode) && finalNode.GetBoolean();
        return new BrowserEvent(isFinal ? "final" : "partial", Text: text, IsFinal: isFinal);
    }
}

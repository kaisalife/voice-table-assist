using System.Net.WebSockets;
using System.Runtime.InteropServices;
using System.Text;
using VoiceTableAssist.Infrastructure;
using VoiceTableAssist.Services;

namespace VoiceTableAssist.Asr;

/// <summary>
/// 浏览器 WS ↔ 进程内 sherpa-onnx 的桥（不再有 sherpa 子进程与上游 WS）。
///
/// 协议不变：浏览器上行二进制 float32 PCM（16kHz），下行 JSON 事件
/// loading / ready / partial / final / cells / error。
///
/// 关键差异（对齐安卓方案）：
///   - 识别器进程内常驻，模型加载一次；**热词按流传入**（本表词表随连接绑定），
///     切表/导入都不重建识别器、也不再有"重启期间语音不可用"；
///   - 端点检测在此循环内判定：端点 → 输出 final 并复位该流（可继续下一句）。
///
/// 可选挂接 VoiceInteractionSession：服务端静默自动提交并下发解析好的 cells。
/// </summary>
internal static class SherpaAsrBridge
{
    public static async Task RunAsync(
        HttpContext context,
        SherpaRecognizerHost engine,
        string? hotwords,
        SoundAligner? aligner,
        CancellationToken aborted,
        CancellationToken takeover,
        GtcrnDenoiser? denoiser = null,
        Func<ConnectionSender, VoiceInteractionSession>? sessionFactory = null)
    {
        var logger = context.RequestServices.GetRequiredService<ILoggerFactory>().CreateLogger("AsrGateway");
        var host = context.RequestServices.GetRequiredService<EngineHost>();
        using var browser = await context.WebSockets.AcceptWebSocketAsync();
        using var sender = new ConnectionSender(browser);
        using var interaction = sessionFactory?.Invoke(sender);
        using var session = CancellationTokenSource.CreateLinkedTokenSource(aborted, takeover);

        try
        {
            // 懒加载：首次在此装载语义引擎（RaNER/嵌入）与语音识别器，期间向浏览器发 loading 进度帧。
            await host.EnsureEnginesAsync(msg => SendLoading(sender, msg, session.Token));
            await engine.EnsureReadyAsync(msg => SendLoading(sender, msg, session.Token));

            // 本表热词随流传入（'/' 分隔）：识别器常驻，导入/切表后新连接自动生效。
            using var stream = engine.CreateStream(hotwords);
            var hotwordCount = string.IsNullOrEmpty(hotwords) ? 0 : hotwords.Split('/').Length;
            logger.LogInformation("WS 会话：进程内识别器就绪（sherpa {Version}，本表热词 {Count} 条）", engine.Version, hotwordCount);

            await sender.SendAsync(new BrowserEvent("ready"), session.Token);

            // 降噪器是进程级单例：每通会话开始必须复位（GRU/STFT/overlap-add 状态），
            // 否则上一通对话的尾帧会串进本通的开头几帧。
            denoiser?.Reset();

            var audioBytes = 0L;
            string? lastPartial = null;

            while (browser.State == WebSocketState.Open && sender.IsOpen)
            {
                var message = await WsHelper.ReceiveCompleteMessageAsync(browser, session.Token);
                if (message is null) break;

                if (message.Value.Type == WebSocketMessageType.Text)
                {
                    var command = Encoding.UTF8.GetString(message.Value.Payload);
                    if (command.Contains("\"type\":\"stop\"", StringComparison.OrdinalIgnoreCase))
                    {
                        await FinishAsync(stream, sender, interaction, aligner, logger, lastPartial, denoiser, session.Token);
                        return;
                    }
                    continue;
                }
                if (message.Value.Type != WebSocketMessageType.Binary) continue;

                audioBytes += message.Value.Payload.Length;
                var samples = ToSamples(message.Value.Payload, denoiser);
                if (samples.Length == 0) continue;

                stream.Accept(samples);
                var (item, isFinal) = Pump(stream, aligner, logger);
                if (isFinal)
                {
                    if (item is not null)
                    {
                        if (interaction is not null) item = item with { Accumulated = interaction.OnFinal(item.Text!) };
                        lastPartial = null;   // final 已覆盖前面 partial
                        await sender.SendAsync(item, session.Token);
                    }
                    stream.Reset();           // 端点后复位，继续下一句
                }
                else if (item is not null)
                {
                    lastPartial = item.Text;
                    if (interaction is not null) item = item with { Accumulated = interaction.Accumulated };
                    await sender.SendAsync(item, session.Token);
                }
            }

            if (audioBytes > 0)
                logger.LogInformation("WS 会话结束：收到浏览器音频 {KB:F0} KB（约 {Sec:F1}s）", audioBytes / 1024.0, audioBytes / 4.0 / engine.SampleRate);
            else
                logger.LogWarning("WS 会话结束：浏览器未上行任何音频数据（检查麦克风采集是否启动）");
        }
        catch (OperationCanceledException) when (session.IsCancellationRequested) { }
        catch (Exception exception)
        {
            await WsHelper.TrySendErrorAsync(browser, "ASR_CONNECTION", exception.Message);
        }
        finally
        {
            await WsHelper.TryCloseAsync(browser);
        }
    }

    /// <summary>排空解码 → 纠错 → 组装事件；返回 (事件, 是否端点收句)。端点且文本非空时 lastPartial 由调用方清空。</summary>
    private static (BrowserEvent? Item, bool IsFinal) Pump(SherpaStream stream, SoundAligner? aligner, ILogger logger)
    {
        var text = stream.PumpDecode();
        var endpoint = stream.IsEndpoint();
        if (string.IsNullOrEmpty(text))
            return (null, endpoint);

        // 纠错分层（与安卓语音方案一致）：
        //   ① 通用数字同音归一（实/石/时→十、灵→零、付→负…，仅数字上下文）
        //   ② 表内读音吸附：主体/位置按读音吸附回本表词表。
        // partial 与 final 都纠——界面"实时识别"看到的就是最终用来解析的文本。
        var raw = text;
        var corrected = DomainCorrection.Correct(raw);
        if (aligner is { Ready: true })
        {
            var aligned = aligner.AlignSentence(corrected);
            if (aligned != corrected) corrected = aligned;
        }
        if (corrected != raw) logger.LogInformation("[ASR] raw({Kind}): {Raw}", endpoint ? "final" : "partial", raw);

        return (new BrowserEvent(endpoint ? "final" : "partial", Text: corrected, IsFinal: endpoint), endpoint);
    }

    /// <summary>stop 收尾：降噪尾帧 → InputFinished → 排空解码 → 最后一句 final → 提交（静默定时器不再兜底）。</summary>
    private static async Task FinishAsync(
        SherpaStream stream,
        ConnectionSender sender,
        VoiceInteractionSession? interaction,
        SoundAligner? aligner,
        ILogger logger,
        string? lastPartial,
        GtcrnDenoiser? denoiser,
        CancellationToken cancellationToken)
    {
        try
        {
            if (denoiser is not null)
            {
                var tail = denoiser.Denoise(Array.Empty<float>(), flush: true);
                if (tail.Length > 0) stream.Accept(tail);
            }
        }
        catch (Exception ex) { logger.LogWarning(ex, "[ASR] 降噪收尾失败（忽略）"); }

        stream.InputFinished();
        var (item, _) = Pump(stream, aligner, logger);
        if (item is not null)
        {
            if (interaction is not null) item = item with { Accumulated = interaction.OnFinal(item.Text!) };
            await sender.SendAsync(item, cancellationToken);
        }

        // 最近一条还没进累计的 partial 补 fold：端点未切句就停止时，累计不会漏掉最后一次识别。
        interaction?.FoldPartial(lastPartial);
        if (interaction is not null) await interaction.FlushAsync(cancellationToken);
    }

    /// <summary>上行 PCM：启用 GTCRN 时先降噪，否则原样解码为 float[]。</summary>
    private static float[] ToSamples(ReadOnlyMemory<byte> payload, GtcrnDenoiser? denoiser)
    {
        // 浏览器统一上行 float32 小端 PCM；字节数按 4 对齐（非 4 倍的残块丢弃）
        var count = payload.Length / 4;
        if (count == 0) return Array.Empty<float>();
        var floats = MemoryMarshal.Cast<byte, float>(payload.Span[..(count * 4)]).ToArray();
        return denoiser is null ? floats : denoiser.Denoise(floats);
    }

    private static void SendLoading(ConnectionSender sender, string message, CancellationToken cancellationToken)
    {
        try { _ = sender.SendAsync(new BrowserEvent("loading", Message: message), cancellationToken); }
        catch (Exception) { /* 进度帧失败不影响主流程 */ }
    }
}

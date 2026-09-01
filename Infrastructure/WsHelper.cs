using System.Net.WebSockets;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace VoiceTableAssist.Infrastructure;

// --------------------------- WebSocket 通用工具 ---------------------------

internal static class WsHelper
{
    // 一条 WebSocket 逻辑消息可能分成多个 frame，必须读到 EndOfMessage 再解析。
    public static async Task<(WebSocketMessageType Type, byte[] Payload)?> ReceiveCompleteMessageAsync(
        WebSocket socket,
        CancellationToken cancellationToken)
    {
        var buffer = new byte[64 * 1024];
        using var output = new MemoryStream();
        WebSocketReceiveResult result;
        do
        {
            result = await socket.ReceiveAsync(buffer, cancellationToken);
            if (result.MessageType == WebSocketMessageType.Close) return null;
            output.Write(buffer, 0, result.Count);
        } while (!result.EndOfMessage);
        return (result.MessageType, output.ToArray());
    }

    public static Task SendJsonAsync(WebSocket socket, BrowserEvent item, CancellationToken cancellationToken)
    {
        var json = JsonSerializer.SerializeToUtf8Bytes(item, new JsonSerializerOptions
        {
            PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
            DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull
        });
        return socket.SendAsync(json, WebSocketMessageType.Text, true, cancellationToken);
    }

    public static Task SendTextAsync(WebSocket socket, string text, CancellationToken cancellationToken) =>
        socket.SendAsync(Encoding.UTF8.GetBytes(text), WebSocketMessageType.Text, true, cancellationToken);

    public static async Task TrySendErrorAsync(WebSocket socket, string code, string message)
    {
        if (socket.State != WebSocketState.Open) return;
        try { await SendJsonAsync(socket, new BrowserEvent("error", Code: code, Message: message), CancellationToken.None); }
        catch { /* 原连接不可写时仅保留服务端日志。 */ }
    }

    public static async Task TryCloseAsync(WebSocket socket)
    {
        if (socket.State is not (WebSocketState.Open or WebSocketState.CloseReceived)) return;
        try { await socket.CloseAsync(WebSocketCloseStatus.NormalClosure, "Session ended", CancellationToken.None); }
        catch { /* 关闭异常不覆盖原始会话异常。 */ }
    }

    public static async Task IgnoreCancellationAsync(params Task[] tasks)
    {
        try { await Task.WhenAll(tasks); }
        catch (OperationCanceledException) { }
    }
}
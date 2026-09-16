using System.Text.Json;
using VoiceTableAssist.Asr;
using VoiceTableAssist.Services;

namespace VoiceTableAssist.Endpoints;

/// <summary>重建语音资源端点（保留给外部调用；/import_table 已进程内直调同一逻辑）。缺省目标=当前活动表。</summary>
internal static class VoiceResourceEndpoints
{
    public static void MapVoiceResourceEndpoints(this WebApplication app)
    {
        app.MapPost("/api/table/voice", async (HttpRequest req, IConfiguration configuration, TableVectorManager manager) =>
        {
            var doc = await JsonDocument.ParseAsync(req.Body, cancellationToken: req.HttpContext.RequestAborted);
            var root = doc.RootElement;

            var rows = new List<string>();
            foreach (var r in root.GetProperty("rows").EnumerateArray())
                rows.Add(r.GetString() ?? "");
            var columnCount = root.TryGetProperty("columnCount", out var cc) ? cc.GetInt32() : 0;
            rows.RemoveAll(string.IsNullOrWhiteSpace);

            var tableKey = root.TryGetProperty("tableKey", out var tk) ? tk.GetString() : manager.LastDatabase;
            var voice = TableVoiceResourceGenerator.Rebuild(configuration, rows, columnCount, tableKey);
            if (!voice.Ok) return Results.BadRequest(new { error = voice.Error });

            // 热词按流传入进程内识别器，导入即对新连接生效——无需重启、无不可用窗口。
            Console.WriteLine($"[VOICE] 本表热词已重建: {Path.Combine(voice.TableDir!, "hotwords.txt")} (key={tableKey ?? "default"} {rows.Count}行/{columnCount}列)");
            return Results.Ok(new { status = "ok", rowsCount = rows.Count, columnCount, tableKey });
        });
    }
}
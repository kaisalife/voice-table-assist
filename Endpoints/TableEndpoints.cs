using System.Text.Json;
using VoiceTableAssist.Services;

namespace VoiceTableAssist.Endpoints;

/// <summary>多表管理端点：GET /tables（清单+活动表）、POST /api/table/unload（显式卸载）。</summary>
internal static class TableEndpoints
{
    public static void MapTableEndpoints(this WebApplication app)
    {
        app.MapGet("/tables", (TableVectorManager manager) => Results.Ok(manager.List()));

        app.MapPost("/api/table/unload", async (HttpRequest req, TableVectorManager manager) =>
        {
            string? tableName = null;
            try
            {
                using var doc = await JsonDocument.ParseAsync(req.Body, cancellationToken: req.HttpContext.RequestAborted);
                if (doc.RootElement.TryGetProperty("tableName", out var n))
                    tableName = n.GetString();
            }
            catch (Exception) { /* 无 body / 非法 body → 视为卸载当前活动表 */ }

            manager.Unload(tableName);
            return Results.Ok(new { status = "ok", activeTable = manager.ActiveTable });
        });
    }
}
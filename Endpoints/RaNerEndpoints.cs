using System.Text.Json;
using VoiceTableAssist.Dtos;
using VoiceTableAssist.Services;

namespace VoiceTableAssist.Endpoints;

/// <summary>文本→表格单元格 与 表导入端点（进程内 RaNER + gte-base-zh，按表名切换）。</summary>
internal static class RaNerEndpoints
{
    public static void MapRaNerEndpoints(this WebApplication app)
    {
        // ---- 文本→表格单元格（保持原响应协议：顶层纯数组）----
        app.MapPost("/text_to_json", async (HttpRequest req) =>
        {
            var host = req.HttpContext.RequestServices.GetRequiredService<EngineHost>();
            var manager = req.HttpContext.RequestServices.GetRequiredService<TableVectorManager>();

            string? text; string? table;
            try
            {
                using var doc = await JsonDocument.ParseAsync(req.Body);
                var root = doc.RootElement;
                if (!root.TryGetProperty("text", out var textNode) || string.IsNullOrWhiteSpace(textNode.GetString()))
                    return Results.Json(new { error = "缺少 text 字段" }, statusCode: 400);
                text = textNode.GetString();
                table = root.TryGetProperty("table", out var t) ? t.GetString() : null;
            }
            catch (JsonException ex)
            {
                return Results.Json(new { error = $"请求体不是合法 JSON：{ex.Message}" }, statusCode: 400);
            }

            var cells = new List<CellDto>();
            try
            {
                await host.EnsureEnginesAsync();   // 懒加载：首次查询时装载模型（数秒）
                var idx = manager.Activate(table); // 显式表名=激活它；省略=当前活动表→default
                var bio = host.Raner.Predict(text!);
                var triples = TripleExtractor.Extract(bio);
                foreach (var (sub, obj, val) in triples)
                {
                    var phrase = sub + obj;
                    var (row, col, _, _) = host.Embed.Lookup(phrase, idx);
                    if (row <= 0) continue;   // 向量检索低置信未命中，跳过
                    cells.Add(new CellDto
                    {
                        row = row,
                        column = col,
                        values = ChineseNumeral.ToDecimal(val),
                    });
                }
            }
            catch (KeyNotFoundException ex)
            {
                return Results.Json(new { error = ex.Message }, statusCode: 404);
            }
            catch (Exception ex)
            {
                return Results.Json(new { error = ex.Message }, statusCode: 500);
            }

            return Results.Json(cells);
        });

        // ---- 表导入：初始化按表 embedding 向量库 + 更新 registry + 激活 + 进程内重建该表语音资源 ----
        app.MapPost("/import_table", async (HttpRequest req) =>
        {
            var manager = req.HttpContext.RequestServices.GetRequiredService<TableVectorManager>();

            using var doc = await JsonDocument.ParseAsync(req.Body);
            var root = doc.RootElement;

            var rows = new List<RowDef>();
            foreach (var r in root.GetProperty("rows").EnumerateArray())
                // index 已弃用：向量库坐标一律按 rows 数组顺序生成，请求里的 index 仅兼容旧客户端
                rows.Add(new RowDef { Label = r.GetProperty("label").GetString()!, Index = r.TryGetProperty("index", out var ix) ? ix.GetInt32() : 0 });

            var columnCount = root.GetProperty("columnCount").GetInt32();

            var tableName = root.TryGetProperty("tableName", out var tn) ? tn.GetString() : null;
            if (string.IsNullOrWhiteSpace(tableName)) tableName = manager.DefaultTable;

            try
            {
                var summary = manager.Import(tableName, rows, columnCount);
                return Results.Ok(new
                {
                    status = "ok",
                    tableName,
                    rowsCount = summary.RowsCount,
                    colsCount = summary.ColsCount,
                    entries = summary.Entries,
                    dim = summary.Dim,
                });
            }
            catch (Exception ex)
            {
                return Results.Json(new { error = ex.Message }, statusCode: 400);
            }
        });
    }
}
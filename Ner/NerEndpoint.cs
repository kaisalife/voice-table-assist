using VoiceTableAssist.Services;

namespace VoiceTableAssist.Ner;

/// <summary>
/// NER 推理端点注册（进程内直调 RaNER + gte-base-zh，替代原外部 Node.js HTTP 依赖）。
/// POST /api/speech/ner → 将语音识别文本解析为结构化坐标列表；body 可选 table 指定目标表。
/// </summary>
internal static class NerEndpoint
{
    public static void MapNerEndpoint(this WebApplication app)
    {
        app.MapPost("/api/speech/ner", async (HttpContext context) =>
        {
            try
            {
                var request = await context.Request.ReadFromJsonAsync<NerRequest>();
                if (request is null || string.IsNullOrWhiteSpace(request.Text))
                {
                    context.Response.StatusCode = StatusCodes.Status400BadRequest;
                    await context.Response.WriteAsJsonAsync(new { error = "缺少 text 字段" });
                    return;
                }

                var host = context.RequestServices.GetRequiredService<EngineHost>();
                var manager = context.RequestServices.GetRequiredService<TableVectorManager>();

                var sw = System.Diagnostics.Stopwatch.StartNew();
                await host.EnsureEnginesAsync();   // 懒加载：首次查询时装载模型（数秒）
                var idx = manager.Activate(request.Table);
                var bio = host.Raner.Predict(request.Text);
                var triples = TripleExtractor.Extract(bio);

                // 过滤：缺数值（"?")或无法定位到单元格的三元组不构成本接口的有效结果
                var valid = new List<NerTriple>();
                foreach (var (sub, obj, val) in triples)
                {
                    if (val == "?") continue;
                    var phrase = sub + obj;
                    var (row, col, _, _) = host.Embed.Lookup(phrase, idx);
                    if (row < 0 || col < 0) continue;
                    valid.Add(new NerTriple(Column: col, Row: row, Value: ChineseNumeral.ToDecimal(val)));
                }
                sw.Stop();

                if (valid.Count == 0)
                {
                    context.Response.StatusCode = StatusCodes.Status422UnprocessableEntity;
                    await context.Response.WriteAsJsonAsync(new
                    {
                        error = "未能从文本中解析出有效的三元组",
                        text = request.Text
                    });
                    return;
                }

                context.Response.StatusCode = StatusCodes.Status200OK;
                await context.Response.WriteAsJsonAsync(new
                {
                    triples = valid,
                    elapsedMs = (int)sw.ElapsedMilliseconds
                });
            }
            catch (Exception ex)
            {
                context.Response.StatusCode = StatusCodes.Status500InternalServerError;
                await context.Response.WriteAsJsonAsync(new
                {
                    error = "NER 推理失败",
                    detail = ex.Message
                });
            }
        });
    }
}
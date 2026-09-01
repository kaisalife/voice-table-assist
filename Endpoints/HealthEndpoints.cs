using VoiceTableAssist.Infrastructure;
using VoiceTableAssist.Services;

namespace VoiceTableAssist.Endpoints;

/// <summary>健康检查端点（合并 backend /api/health 与 text_to_json /healthz），增补 activeTable。</summary>
internal static class HealthEndpoints
{
    public static void MapHealthEndpoints(this WebApplication app)
    {
        app.MapGet("/api/health", (ModelPaths paths, TableVectorManager manager, EngineHost host) =>
        {
            return Results.Ok(new
            {
                status = "ok",
                service = "voice-table-assist",
                provider = "sherpa",
                configured = true,
                ranerModelDir = paths.ModelDir,
                activeTable = manager.ActiveTable,
                modelsLoaded = host.IsLoaded,   // 懒加载模式下空闲时为 false（待机省内存）
                provider_version = "1"
            });
        });

        // 健康检查别名（兼容原 text_to_json 调用方）
        app.MapGet("/healthz", (ModelPaths paths, TableVectorManager manager, EngineHost host) =>
            Results.Ok(new
            {
                status = "ok",
                modelDir = paths.ModelDir,
                mode = "RaNER+gte-base-zh",
                provider = "sherpa",
                configured = true,
                activeTable = manager.ActiveTable,
                modelsLoaded = host.IsLoaded
            }));
    }
}
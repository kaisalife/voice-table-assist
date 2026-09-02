using System.Security.Cryptography.X509Certificates;
using VoiceTableAssist.Asr;
using VoiceTableAssist.Endpoints;
using VoiceTableAssist.Infrastructure;
using VoiceTableAssist.Ner;
using VoiceTableAssist.Services;

// 合并后的单一自包含服务包：sherpa-onnx(离线 ASR) + 进程内 RaNER/gte-base-zh 单进程单端口对外。
// 本地开发可把覆盖项写进 app/.env（或 .env）；正式部署推荐 appsettings.json 或系统环境变量。
DotEnv.Load();

// 内容根固定为可执行文件目录：appsettings.json / wwwroot / certs 不随"工作目录"漂移，
// 无论前台、Windows 服务、systemd 还是从别处启动都能正确加载。
// 必须通过 CreateBuilder 初始参数设定：builder 创建后再改 Host 配置会抛 NotSupportedException。
var builder = WebApplication.CreateBuilder(new WebApplicationOptions
{
    Args = args,
    ContentRootPath = AppContext.BaseDirectory,
});

// 服务托管：Windows 注册为服务（sc.exe / nssm）；Linux 走 systemd（前台运行行为不变）。
if (OperatingSystem.IsWindows())
    builder.Host.UseWindowsService(options => options.ServiceName = "VoiceTableAssist");
else if (OperatingSystem.IsLinux())
    builder.Host.UseSystemd();

// ---- HTTPS：平板浏览器直访入口（替代 Cordova 壳的推荐方式）----
// certs/gateway.pfx 存在时（make-cert.bat 生成，私钥密码见下）追加 https://0.0.0.0:15433。
// https 页面属于安全上下文，getUserMedia（麦克风）直接可用；前端零改动
// （voice-mic.js 按 location.protocol 自适应 ws/wss，请求均为相对路径）。
// 内网自签证书：密码写死即可，不构成公网暴露面；IP 变更后重跑 make-cert.bat 重签。
const string HttpsCertPassword = "vta-local-2026";
var certPfxPath = Path.Combine(AppContext.BaseDirectory, "certs", "gateway.pfx");
if (File.Exists(certPfxPath))
{
    var httpUrls = builder.Configuration["Urls"] ?? "http://0.0.0.0:15232";
    builder.WebHost.UseUrls(httpUrls, "https://0.0.0.0:15433");
    builder.WebHost.ConfigureKestrel(o => o.ConfigureHttpsDefaults(h =>
        h.ServerCertificate = new X509Certificate2(certPfxPath, HttpsCertPassword)));
    Console.WriteLine("[HTTPS] certs/gateway.pfx 已加载 -> https://0.0.0.0:15433 （平板：安装 certs/ca.crt 后直访）");
}
else
{
    Console.WriteLine("[HTTPS] 未找到 certs/gateway.pfx，仅 HTTP。平板浏览器需麦克风时先运行 make-cert.bat。");
}

// 自动管理 sherpa-onnx 进程生命周期：注册具体类型供 EngineHost 按需启停（懒加载），
// 同时挂 IHostedService 兜底退出清理。
builder.Services.AddSingleton<SherpaServerManager>();
builder.Services.AddHostedService(sp => sp.GetRequiredService<SherpaServerManager>());

// 服务模式下的控制台日志不可见，落盘到日志目录便于排查。
// 按天滚动；启动时自动清理超过 Logging:RetentionDays（默认 30 天）的旧日志，防长期运行撑爆磁盘。
builder.Logging.AddProvider(new FileLoggerProvider(
    Path.Combine(AppContext.BaseDirectory, "logs"),
    builder.Configuration.GetValue("Logging:RetentionDays", 30)));

// CORS：供平板/其他页面跨源调用 /text_to_json 等 HTTP 接口。
builder.Services.AddCors(o => o.AddDefaultPolicy(p => p.AllowAnyOrigin().AllowAnyMethod().AllowAnyHeader()));

// 进程内 RaNER + gte-base-zh 加载/自检/注册（模型目录解析见 ModelBootstrap）。
ModelBootstrap.Register(builder.Services, builder.Configuration);

// 多表向量管理器 + 空闲卸载监控。
builder.Services.AddSingleton<TableVectorManager>();
builder.Services.AddHostedService<TableIdleMonitor>();

var app = builder.Build();

// 启动即激活 default 表（读旧 cell_index.json 或 registry），再自检。
var tvm = app.Services.GetRequiredService<TableVectorManager>();
tvm.Activate(tvm.DefaultTable);
ModelBootstrap.RunSelfCheck(app.Services);

// 浏览器与网关之间是长连接；ASR 供应商密钥只在服务端读取，不返回给前端。
app.UseWebSockets(new WebSocketOptions { KeepAliveInterval = TimeSpan.FromSeconds(20) });

// CORS + 本地验证页（wwwroot）：http://localhost:15232/
app.UseCors();
app.UseDefaultFiles();
// 前端文件更新后浏览器必须立刻拿到新版：只协商缓存（no-cache），不禁用缓存本体。
app.UseStaticFiles(new StaticFileOptions
{
    OnPrepareResponse = ctx =>
        ctx.Context.Response.Headers[Microsoft.Net.Http.Headers.HeaderNames.CacheControl] = "no-cache",
});

// ---- 路由：单端口 15232 ----
app.MapHealthEndpoints();
app.MapRaNerEndpoints();
app.MapVoiceResourceEndpoints();
app.MapTableEndpoints();

// ---- 语音识别流式端点（?table= 连接即激活对应表）----
// 服务端交互编排：final 文本静默自动提交，直发 type=cells 的解析结果，
// 前端只负责开关麦克风和收结果。Interaction:SilenceMs 可调（默认 2500ms）。
app.Map("/api/speech/asr/stream", async (HttpContext context) =>
{
    if (!context.WebSockets.IsWebSocketRequest)
    {
        context.Response.StatusCode = StatusCodes.Status400BadRequest;
        await context.Response.WriteAsJsonAsync(new { error = "WebSocket request required." });
        return;
    }

    var configuration = context.RequestServices.GetRequiredService<IConfiguration>();
    var logger = context.RequestServices.GetRequiredService<ILoggerFactory>().CreateLogger("VoiceInteraction");
    var manager = context.RequestServices.GetRequiredService<TableVectorManager>();

    var table = context.Request.Query["table"].FirstOrDefault();
    // 表解析是纯内存快操作，留在握手路径（未注册 → 404，异常 → 500 带原因）。
    string tableKey;
    try { tableKey = manager.ResolveTargetKey(table); }
    catch (KeyNotFoundException)
    {
        context.Response.StatusCode = StatusCodes.Status404NotFound;
        await context.Response.WriteAsJsonAsync(new { error = $"表 \"{table}\" 尚未导入，请先 POST /import_table" });
        return;
    }
    catch (Exception ex)
    {
        logger.LogError(ex, "表 {Table} 解析失败", table);
        context.Response.StatusCode = StatusCodes.Status500InternalServerError;
        await context.Response.WriteAsJsonAsync(new { error = $"表 \"{table}\" 解析失败：{ex.Message}" });
        return;
    }

    var host = context.RequestServices.GetRequiredService<EngineHost>();
    // 单连接门卫："新连接接管旧连接"取代 409 拒接——
    // 单用户场景下，按按钮发起的新连接即代表旧连接已过时，直接取消旧会话并放行本次连接，
    // 避免旧连接的半死/残留 socket 锁死会话位，用户任何时刻都能重新开始语音录入。
    var sessionCts = host.AcquireSession();
    try
    {
        // HR 同音纠正规则按 key 加载（小文件 + 缓存，毫秒级），随连接捕获不随切表漂移。
        // 握手同步激活该表：唯一会话期间活动表不会变化，会话提交时直接查当前活动索引，无需再等待快照。
        var replacer = HomophoneReplacerProvider.Get(configuration, tableKey);
        manager.Activate(tableKey);
        var sessionFactory = ConnectionSender.CreateFactory(configuration, context.RequestServices, logger, tableKey);
        await SherpaAsrBridge.RunAsync(context, SherpaOptions.From(configuration), replacer, context.RequestAborted, sessionCts.Token, sessionFactory);
    }
    finally
    {
        host.ReleaseSession(sessionCts);
    }
});

// ---- NER 推理端点（进程内 RaNER，保持 backend 既有调用方兼容）----
app.MapNerEndpoint();

Console.WriteLine("[RANER] 服务已启动: WS /api/speech/asr/stream?table= , POST /text_to_json , POST /import_table{tableName} , POST /api/speech/ner , POST /api/table/voice , GET /tables , POST /api/table/unload , GET /api/health /healthz");
app.Run();
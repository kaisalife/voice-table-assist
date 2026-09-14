// VtaNative.cs —— VTA 语音表格插件 .NET8 DllImport 客户端（单文件，无第三方依赖）
//
// 接入三步（详见《安卓化方案接口适配文档.md》）：
//   1) csproj 引入：libvta-service.so（AndroidNativeLibrary，arm64-v8a/x86_64 两份）
//      + 模型资产（AndroidAsset，models/**），声明 RECORD_AUDIO 权限；
//   2) 首次启动：把 assets 里的 models/** 复制到 filesDir/vta（MAUI 用 FileSystem.OpenAppPackageFileAsync）；
//   3) 调用：new VtaClient(dataDir) → ImportTable → Open → 轮询 PollState/PollCells → Close。
//
// 语义：单会话（一个麦克风 = 一路识别）。Open 若已有活动会话，先自动关闭再开新的。
// CellHit 的 Row/Column 为 0-indexed（与 H5 表格第 0 行第 0 列直接对应）。
// 线程约束：PollCells 在无数据时服务端最多挂 250ms——禁止在 UI 线程调用。
using System;
using System.Runtime.InteropServices;
using System.Text;

namespace Vta
{
    /// <summary>扁平 C ABI（libvta-service.so）。一般直接用 <see cref="VtaClient"/>。</summary>
    internal static class VtaNative
    {
        private const string Lib = "libvta-service.so";

        [DllImport(Lib)] internal static extern int vta_start(string dataDir);
        [DllImport(Lib)] internal static extern int vta_version();
        [DllImport(Lib)] internal static extern int vta_health(StringBuilder buf, int cap);
        [DllImport(Lib)] internal static extern int vta_list_tables(StringBuilder buf, int cap);
        [DllImport(Lib)] internal static extern int vta_import(string name, string rowsText, int columnCount);
        [DllImport(Lib)] internal static extern int vta_open(string table, int silenceMs, int captureMode);
        [DllImport(Lib)] internal static extern int vta_push_pcm(int sid, float[] samples, int n);
        [DllImport(Lib)] internal static extern int vta_state(int sid, StringBuilder buf, int cap);
        [DllImport(Lib)] internal static extern int vta_cells(int sid, int maxN, StringBuilder buf, int cap);
        [DllImport(Lib)] internal static extern int vta_close(int sid);
        [DllImport(Lib)] internal static extern int vta_set_denoise(int on);
        [DllImport(Lib)] internal static extern void vta_shutdown();
    }

    /// <summary>错误码：0 成功；-2 参数非法/表未导入；-3 模型未就绪；-4 内部错误。</summary>
    public static class VtaError
    {
        public const int Ok = 0;
        public const int Invalid = -2;
        public const int NotReady = -3;
        public const int Internal = -4;
    }

    /// <summary>一次「说完了」的解析结果。Row/Column 0-indexed。</summary>
    public sealed class CellHit
    {
        public int Row { get; set; }
        public int Column { get; set; }
        public string Value { get; set; } = "";
        public string Raw { get; set; } = "";
        public long FinalizedAtMs { get; set; }
    }

    /// <summary>会话状态轮询结果。Partial 已过全部纠错（界面所见=所填）。</summary>
    public sealed class SessionState
    {
        public int SessionId { get; set; }
        public string TableName { get; set; } = "";
        public string Phase { get; set; } = "";   // idle/listening/endpointing/closed
        public string Partial { get; set; } = "";
        public long LastPartialMs { get; set; }
    }

    /// <summary>VTA 语音表格客户端：进程内单会话，函数调用即接入。</summary>
    public sealed class VtaClient : IDisposable
    {
        private readonly string _token;

        /// <param name="token">接入令牌（与插件约定一致；插件校验失败返回 401 场景仅 HTTP 形态，本形态预留）</param>
        public VtaClient(string token = "") { _token = token; }

        /// <summary>初始化引擎。dataDir = 模型根目录（主应用 filesDir/vta）。幂等。</summary>
        public int Start(string dataDir)
        {
            var rc = VtaNative.vta_start(dataDir);
            if (rc != VtaError.Ok) return rc;
            using var sb = new StringBuilder(256);
            return VtaNative.vta_health(sb, sb.Capacity) <= sb.Capacity ? VtaError.Ok : VtaError.Internal;
        }

        public int Version() => VtaNative.vta_version();

        /// <summary>导入表：rows = 行标签（顺序即行号），cols = 列数。同步构建索引；幂等（同名覆盖）。</summary>
        public int ImportTable(string name, string[] rows, int cols) =>
            VtaNative.vta_import(name, string.Join("\n", rows), cols);

        /// <summary>打开会话（captureMode: 0=插件自采音[推荐]，1=主应用 PushPcm 喂流）。
        /// 已有活动会话会先自动关闭。返回 sessionId。</summary>
        public int Open(string table, int silenceMs = 300, int captureMode = 0) =>
            VtaNative.vta_open(table, silenceMs, captureMode);

        /// <summary>喂 PCM（captureMode=1）：16kHz float32 单声道，n ≤ 4096。</summary>
        public int PushPcm(int sid, float[] samples) => VtaNative.vta_push_pcm(sid, samples, samples.Length);

        /// <summary>轮询状态（后台线程，50~100ms 一次）。</summary>
        public SessionState PollState(int sid)
        {
            using var sb = new StringBuilder(8192);
            VtaNative.vta_state(sid, sb, sb.Capacity);
            return System.Text.Json.JsonSerializer.Deserialize<SessionState>(sb.ToString())
                   ?? new SessionState { SessionId = sid, Phase = "closed" };
        }

        /// <summary>拉取解析结果（后台线程；无数据服务端最多挂 250ms）。</summary>
        public CellHit[] PollCells(int sid, int max = 16)
        {
            using var sb = new StringBuilder(16384);
            VtaNative.vta_cells(sid, max, sb, sb.Capacity);
            return System.Text.Json.JsonSerializer.Deserialize<CellHit[]>(sb.ToString())
                   ?? Array.Empty<CellHit>();
        }

        /// <summary>停止并提交（flush + 剩余 cells；调用后请再 PollCells 一次拿最后一批）。</summary>
        public int Close(int sid) => VtaNative.vta_close(sid);

        public int SetDenoise(bool on) => VtaNative.vta_set_denoise(on ? 1 : 0);

        public void Dispose() => VtaNative.vta_shutdown();
    }
}

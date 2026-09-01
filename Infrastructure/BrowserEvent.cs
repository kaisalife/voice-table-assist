using VoiceTableAssist.Dtos;

namespace VoiceTableAssist.Infrastructure;

/// <summary>浏览器事件模型：服务端与前端之间的统一消息格式。</summary>
/// <remarks>
/// type=cells 时 Text=本次提交的完整语句、Cells=解析好的单元格（服务端静默自动提交的结果）。
/// Cells 仅在 cells 事件中非空；序列化时 null 字段一律省略，老事件协议不受影响。
/// Accumulated 仅在 partial/final 事件中携带：服务端交互会话的当前累计文本（调试用，
/// 前端可直接展示"已说了多少"，无需自行合并）。
/// </remarks>
internal sealed record BrowserEvent(
    string Type,
    string? Text = null,
    bool? IsFinal = null,
    int? StartTime = null,
    int? EndTime = null,
    string? Code = null,
    string? Message = null,
    IReadOnlyList<CellDto>? Cells = null,
    string? Accumulated = null);
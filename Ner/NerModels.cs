namespace VoiceTableAssist.Ner;

// ---- 请求/响应 ----

/// <summary>NER 推理请求（来自前端）。Table 可选：null/空→当前活动表→default。</summary>
public sealed record NerRequest(string Text, string? Table = null);

/// <summary>单个三元组（坐标 + 数值）。</summary>
public sealed record NerTriple(int? Column, int? Row, double? Value);
namespace VoiceTableAssist.Dtos;

/// <summary>导入表格初始化向量库的请求。</summary>
public sealed class ImportTableRequest
{
    public List<RowDef> Rows { get; set; } = [];
    public int ColumnCount { get; set; }
}

public sealed class RowDef
{
    /// <summary>行标签（检验内容），如 "外径"。</summary>
    public string Label { get; set; } = "";

    /// <summary>行号，从 1 开始。</summary>
    public int Index { get; set; }
}
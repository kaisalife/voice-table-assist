namespace VoiceTableAssist.Dtos;

/// <summary>一个填充单元格：中文数值已转为阿拉伯数字。</summary>
public sealed class CellDto
{
    public int row { get; set; }
    public int column { get; set; }
    public double values { get; set; }
}
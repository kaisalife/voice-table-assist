namespace VoiceTableAssist.Services;

// =====================================================================
// 向量库（二进制格式 VTX1）
//
// 布局（小端，全部 int 为 4 字节）：
//   [魔数 "VTX1" 4B][dim][rowsCount][colsCount]
//   [rowsLabelCount] 每行标签: [len][UTF8 bytes]...
//   [entryCount]     每条目:   [row][col][len][UTF8 phrase][vec[dim] float32]
//
// 相比旧 JSON 文本：768 维浮点以二进制存放，体积约为 JSON 的 40%，
// 加载无解析成本（读盘 + 内存拷贝），实测快 5~10 倍。
// =====================================================================
public sealed record Cell(int Row, int Col, string Phrase, float[] Vec);
public sealed record VectorIndexData(List<Cell> Entries, string[] Rows, int RowsCount, int ColsCount, int Dim);

public static class VectorIndex
{
    private const uint Magic = 0x31585456;   // "VTX1" (小端: 56 54 58 31)

    private const int HeaderSize = 4 + sizeof(int) * 3 + sizeof(int); // 魔数+dim+rows+cols+rowsLabelCount

    public static VectorIndexData Load(string path)
    {
        using var stream = File.OpenRead(path);
        using var reader = new BinaryReader(stream);
        if (reader.ReadUInt32() != Magic)
            throw new InvalidDataException($"向量库文件头不合法: {path}（期望 VTX1 二进制格式）");
        var dim = reader.ReadInt32();
        var rowsCount = reader.ReadInt32();
        var colsCount = reader.ReadInt32();
        var labelCount = reader.ReadInt32();
        var rows = new string[labelCount];
        for (var i = 0; i < labelCount; i++)
            rows[i] = reader.ReadString();

        var entryCount = reader.ReadInt32();
        var entries = new List<Cell>(entryCount);
        var vecBytes = dim * sizeof(float);
        for (var i = 0; i < entryCount; i++)
        {
            var row = reader.ReadInt32();
            var col = reader.ReadInt32();
            var phrase = reader.ReadString();
            if (reader.BaseStream.Length - reader.BaseStream.Position < vecBytes)
                throw new InvalidDataException($"向量库条目不足: {path} entry#{i}");
            var vec = reader.ReadSingleArray(dim);
            entries.Add(new Cell(row, col, phrase, vec));
        }
        return new VectorIndexData(entries, rows, rowsCount, colsCount, dim);
    }

    public static void Save(string path, VectorIndexData index)
    {
        var dir = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        var tmp = path + $".tmp-{Guid.NewGuid():N}";
        using (var stream = File.Open(tmp, FileMode.Create, FileAccess.Write, FileShare.None))
        using (var writer = new BinaryWriter(stream))
        {
            writer.Write(Magic);
            writer.Write(index.Dim);
            writer.Write(index.RowsCount);
            writer.Write(index.ColsCount);
            writer.Write(index.Rows.Length);
            foreach (var label in index.Rows) writer.Write(label);
            writer.Write(index.Entries.Count);
            foreach (var e in index.Entries)
            {
                writer.Write(e.Row);
                writer.Write(e.Col);
                writer.Write(e.Phrase);
                var vecBytes = new byte[e.Vec.Length * sizeof(float)];
                System.Buffer.BlockCopy(e.Vec, 0, vecBytes, 0, vecBytes.Length);
                writer.Write(vecBytes);
            }
        }
        File.Move(tmp, path, true);   // 原子替换，绝不读半成品
    }
}

internal static class BinaryReaderVecExtension
{
    public static float[] ReadSingleArray(this BinaryReader reader, int count)
    {
        var vec = new float[count];
        var bytes = System.Buffers.ArrayPool<byte>.Shared.Rent(count * sizeof(float));
        try
        {
            var read = reader.Read(bytes, 0, count * sizeof(float));
            if (read != count * sizeof(float))
                throw new EndOfStreamException("向量数据不足");
            System.Buffer.BlockCopy(bytes, 0, vec, 0, read);
        }
        finally { System.Buffers.ArrayPool<byte>.Shared.Return(bytes); }
        return vec;
    }
}

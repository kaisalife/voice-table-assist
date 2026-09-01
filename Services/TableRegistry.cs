using System.Text;
using System.Text.Json;

namespace VoiceTableAssist.Services;

/// <summary>注册表条目（name ↔ 文件系统安全 key 的映射 + 维度元信息）。</summary>
internal sealed record TableEntry(
    string Name, string Key, int RowsCount, int ColsCount, int Dim, DateTime ImportedAt)
{
    public TableEntry WithMeta(int rows, int cols, int dim) =>
        new(Name, Key, rows, cols, dim, DateTime.UtcNow);
}

/// <summary>
/// 多表注册表存储：维护 name↔key 映射清单，读写 registry.json（写时拷贝原子替换）。
/// 损坏时备份 .bak 并重建空清单（不崩）。key 一律经此解析，杜绝路径穿越。
/// </summary>
internal sealed class TableRegistry
{
    private readonly ModelPaths _paths;
    private readonly ILogger _logger;
    private readonly object _gate = new();
    private List<TableEntry> _entries = new();

    public TableRegistry(ModelPaths paths, ILogger<TableVectorManager> logger)
    {
        _paths = paths;
        _logger = logger;
        Load();
    }

    public string DefaultTable => _paths.DefaultTable;

    /// <summary>线程安全快照（供 List 端点使用）。</summary>
    public List<TableEntry> Snapshot() { lock (_gate) return _entries.ToList(); }

    /// <summary>登记/更新表，返回其 key；新表用 SanitizeTableKey，撞名追加数字后缀。</summary>
    public string EnsureRegistered(string name, int rows, int cols, int dim)
    {
        lock (_gate)
        {
            var existing = _entries.FirstOrDefault(e => string.Equals(e.Name, name, StringComparison.Ordinal));
            if (existing != null)
            {
                _entries.Remove(existing);
                _entries.Add(existing.WithMeta(rows, cols, dim));
                return existing.Key;
            }
            var baseKey = SanitizeTableKey(name);
            var key = baseKey;
            var suffix = 1;
            while (_entries.Any(e => string.Equals(e.Key, key, StringComparison.Ordinal)))
                key = $"{baseKey}_{suffix++}";
            _entries.Add(new TableEntry(name, key, rows, cols, dim, DateTime.UtcNow));
            return key;
        }
    }

    /// <summary>表名→key；未导入（非 default 且无记录）抛 KeyNotFoundException。</summary>
    public string ResolveExistingKey(string name)
    {
        if (string.Equals(name, DefaultTable, StringComparison.Ordinal)) return DefaultTable;
        lock (_gate)
        {
            var e = _entries.FirstOrDefault(x => string.Equals(x.Name, name, StringComparison.Ordinal)
                                              || string.Equals(x.Key, name, StringComparison.Ordinal));
            if (e != null) return e.Key;
        }
        throw new KeyNotFoundException($"表 \"{name}\" 尚未导入，请先 POST /import_table");
    }

    private void Load()
    {
        try
        {
            if (File.Exists(_paths.RegistryPath))
            {
                using var doc = JsonDocument.Parse(File.ReadAllBytes(_paths.RegistryPath));
                _entries = doc.RootElement.GetProperty("tables").EnumerateArray().Select(
                    t => new TableEntry(
                        Prop(t, "name").GetString()!, Prop(t, "key").GetString()!,
                        Prop(t, "rowsCount").GetInt32(), Prop(t, "colsCount").GetInt32(),
                        Prop(t, "dim").GetInt32(), Prop(t, "importedAt").GetDateTime())).ToList();
            }
            else if (File.Exists(_paths.LegacyIndexPath))
            {
                // 首次启动：旧单文件 cell_index.json ↔ 登记 default（向后兼容）
                _entries = [new TableEntry("default", DefaultTable, 0, 0, 0, DateTime.UtcNow)];
                Write();
            }
        }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "[TABLES] registry 损坏，备份后重建空清单");
            try { File.Copy(_paths.RegistryPath, _paths.RegistryPath + ".bak", true); } catch { }
            _entries = new List<TableEntry>();
        }
    }

    /// <summary>属性名兼容读取：优先 camelCase（现行写法），回退 PascalCase（旧版本文件）。
    /// 曾因大小写不一致导致重启后注册表被误判损坏、全部表 404。</summary>
    private static JsonElement Prop(JsonElement element, string camelCase) =>
        element.TryGetProperty(camelCase, out var value) ? value : element.GetProperty(char.ToUpperInvariant(camelCase[0]) + camelCase[1..]);

    public void Write()
    {
        var payload = new { tables = _entries.Select(e => new { name = e.Name, key = e.Key, rowsCount = e.RowsCount, colsCount = e.ColsCount, dim = e.Dim, importedAt = e.ImportedAt }) };
        WriteJsonAtomic(_paths.RegistryPath, JsonSerializer.Serialize(payload));
    }

    /// <summary>写时拷贝：先写临时文件再 File.Move(覆盖)，避免读到半成品。</summary>
    private static void WriteJsonAtomic(string path, string json)
    {
        var dir = Path.GetDirectoryName(path)!;
        Directory.CreateDirectory(dir);
        var tmp = path + $".tmp-{Guid.NewGuid():N}";
        File.WriteAllText(tmp, json);
        File.Move(tmp, path, true);
    }

    /// <summary>生成文件系统安全键：保留中文/数字/下划线/连字符，其余替换为 _（防路径穿越）。</summary>
    public static string SanitizeTableKey(string name)
    {
        var sb = new StringBuilder(name.Length);
        foreach (var ch in name)
        {
            if (ch == '.' || Path.GetInvalidFileNameChars().Contains(ch) || ch is '/' or '\\') sb.Append('_');
            else if (!char.IsWhiteSpace(ch) && (char.IsLetterOrDigit(ch) || ch is '_' or '-' || IsCjk(ch))) sb.Append(ch);
        }
        return string.IsNullOrEmpty(sb.ToString().Trim('_')) ? "table" : sb.ToString().Trim('_');
    }

    private static bool IsCjk(char ch) =>
        (ch >= '\u4E00' && ch <= '\u9FFF') ||   // CJK Unified Ideographs
        (ch >= '\u3400' && ch <= '\u4DBF') ||   // CJK Unified Ideographs Extension A
        (ch >= '\uF900' && ch <= '\uFAFF');     // CJK Compatibility Ideographs
}
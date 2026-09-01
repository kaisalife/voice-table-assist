using VoiceTableAssist.Asr;
using VoiceTableAssist.Dtos;

namespace VoiceTableAssist.Services;

/// <summary>导入结果概要。</summary>
internal sealed record ImportSummary(int RowsCount, int ColsCount, int Entries, int Dim);

/// <summary>
/// 多表向量库 + 语音特化资源管理器（单例）。
/// 按表持久化 embedding 索引（tables/{key}/cell_index.bin 二进制 VTX1 + registry.json），查询/导入按表名切换，
/// 空闲/显式卸载自动清空活动索引（只释放内存，绝不删盘）。key 一律经 TableRegistry 解析，杜绝路径穿越。
/// 单连接模式：语音会话由连入门卫保证唯一，表切换只发生在导入/HTTP 查询之间，用单把锁即可挡住并发。
/// </summary>
internal sealed class TableVectorManager
{
    private readonly EngineHost _host;
    private readonly ModelPaths _paths;
    private readonly IConfiguration _config;
    private readonly ILogger<TableVectorManager> _logger;
    private readonly TableRegistry _registry;
    private readonly SherpaServerManager? _sherpa;
    private readonly SemaphoreSlim _lock = new(1, 1);

    private volatile VectorIndexData? _active;
    public volatile string? LastDatabase;
    private DateTime _lastActiveUtc;

    public TableVectorManager(EngineHost host, ModelPaths paths, IConfiguration config, ILogger<TableVectorManager> logger,
        SherpaServerManager? sherpa = null)
    {
        _host = host;
        _paths = paths;
        _config = config;
        _logger = logger;
        _sherpa = sherpa;
        _registry = new TableRegistry(paths, logger);
    }

    public string DefaultTable => _paths.DefaultTable;
    public string? ActiveTable => LastDatabase;
    public VectorIndexData? ActiveIndex => _active;

    // ---------------- 查询/切换 ----------------

    /// <summary>
    /// 解析目标表 key（快操作，纯内存）：空表名 → 当前活动表 → default；
    /// 未注册的表抛 KeyNotFoundException（调用方转 404）。语义与 Activate 的表名解析一致。
    /// </summary>
    public string ResolveTargetKey(string? tableName)
    {
        if (string.IsNullOrWhiteSpace(tableName))
        {
            if (!string.IsNullOrEmpty(LastDatabase) && _active != null) return LastDatabase;
            tableName = DefaultTable;
        }
        return _registry.ResolveExistingKey(tableName.Trim());
    }

    /// <summary>
    /// 激活指定表并返回其向量索引（同步加载，单连接下握手即等待，秒级）。
    /// 空表名 → 当前活动表 → default；未注册的表抛 KeyNotFoundException（调用方转 404）。
    /// </summary>
    public VectorIndexData? Activate(string? tableName)
    {
        var now = DateTime.UtcNow;
        if (string.IsNullOrWhiteSpace(tableName))
        {
            var cur = _active;
            if (cur != null && LastDatabase != null) { _lastActiveUtc = now; return cur; }
            tableName = DefaultTable;
        }

        var key = _registry.ResolveExistingKey(tableName.Trim());
        _lock.Wait();
        try
        {
            if (string.Equals(LastDatabase, key, StringComparison.Ordinal) && _active != null)
            {
                _lastActiveUtc = now;   // 同名跳过重载，仅刷新活跃时间
                return _active;
            }
            var idx = LoadIndex(key);
            _active = idx;
            LastDatabase = key;
            _lastActiveUtc = now;
            _host.Touch();
            _logger.LogInformation("[TABLES] 激活表 key={Key} ({Rows}x{Cols}@{Dim})", key,
                idx?.RowsCount ?? 0, idx?.ColsCount ?? 0, idx?.Dim ?? 0);
            return idx;
        }
        finally { _lock.Release(); }
    }

    /// <summary>导入表：构建索引 → 写时拷贝落盘 → 更新 registry → 激活 → 重建该表语音资源。</summary>
    public ImportSummary Import(string tableName, IReadOnlyList<RowDef> rows, int columnCount)
    {
        if (string.IsNullOrWhiteSpace(tableName))
            throw new ArgumentException("tableName 不能为空", nameof(tableName));
        var name = tableName.Trim();

        var allPhrases = new List<(int Row, int Col, string Phrase)>();
        foreach (var row in rows)
            for (var col = 1; col <= columnCount; col++)
                foreach (var p in CellPhraseGenerator.Generate(row.Index, row.Label, col))
                    allPhrases.Add((row.Index, col, p));

        // 引擎按需加载（懒加载模式下首次导入时才装载 ONNX 模型）
        _host.EnsureEnginesAsync().GetAwaiter().GetResult();
        var embed = _host.Embed;

        // 批量嵌入：将短语分批交给 ONNX 推理，大幅减少推理调用次数
        var batchSize = _config.GetValue("Embedding:BatchSize", 64);
        var entries = new List<Cell>(allPhrases.Count);
        for (int i = 0; i < allPhrases.Count; i += batchSize)
        {
            var batch = allPhrases.GetRange(i, Math.Min(batchSize, allPhrases.Count - i));
            var texts = batch.Select(p => p.Phrase).ToList();
            var vecs = embed.EmbedBatch(texts);
            for (int j = 0; j < batch.Count; j++)
                entries.Add(new Cell(batch[j].Row, batch[j].Col, batch[j].Phrase, vecs[j]));
        }

        int dim = entries.Count > 0 ? entries[0].Vec.Length : 0;
        var index = new VectorIndexData(entries, rows.Select(r => r.Label).ToArray(),
            rows.Count, columnCount, dim);

        var key = _registry.EnsureRegistered(name, rows.Count, columnCount, dim);
        _lock.Wait();
        try
        {
            var cellPath = CellIndexPath(key);
            // 二进制 VTX1 落盘（内部已原子替换）；顺带清理旧 JSON 文件，不留死文件
            VectorIndex.Save(cellPath, index);
            TryDeleteFile(Path.Combine(CellIndexDir(key), "cell_index.json"));
            _registry.Write();
            _logger.LogInformation("[IMPORT] 表 {Name}(key={Key}) 落盘 {Path}：{Ent}条向量", name, key, cellPath, entries.Count);
        }
        finally { _lock.Release(); }

        Activate(name);
        RebuildVoice(name, key, rows.Select(r => r.Label).ToList(), columnCount);
        return new ImportSummary(rows.Count, columnCount, entries.Count, dim);
    }

    /// <summary>卸载：tableName 空或 == 当前活动表 → 清空活动索引（只释放内存）。</summary>
    public void Unload(string? tableName)
    {
        if (string.IsNullOrWhiteSpace(tableName) || _registry.ResolveExistingKey(tableName.Trim()) == LastDatabase)
        {
            _active = null;
            LastDatabase = null;
            _lastActiveUtc = DateTime.UtcNow;
            _logger.LogInformation("[TABLES] 已卸载活动表");
        }
    }

    public void UnloadIfIdle()
    {
        if (LastDatabase == null) return;
        var timeout = _config.GetValue("Tables:IdleTimeout", TimeSpan.FromMinutes(30));
        if (DateTime.UtcNow - _lastActiveUtc > timeout) Unload(null);
    }

    public object List() => new
    {
        tables = _registry.Snapshot().OrderBy(e => e.Name).Select(e => new
        {
            e.Name, e.Key, e.RowsCount, e.ColsCount, e.Dim, e.ImportedAt,
        }),
        activeTable = LastDatabase,
    };

    // ---------------- 磁盘加载 ----------------

    private void RebuildVoice(string name, string key, IReadOnlyList<string> rowLabels, int columnCount)
    {
        var voice = TableVoiceResourceGenerator.Rebuild(_config, rowLabels, columnCount, key);
        if (!voice.Ok)
        {
            _logger.LogWarning("[IMPORT] 表 {Name} 语音资源重建失败(忽略): {Error}", name, voice.Error);
            return;
        }

        // 热词已聚合到 tables/current/hotwords.txt，但 sherpa 仅在启动时读取——调度后台重启加载。
        ScheduleSherpaRestart($"导入表 {name}");
    }

    private int _restartPending = 0;

    /// <summary>
    /// 调度一次 sherpa 重启（2 秒窗口内合并多次触发，如逐表导入/前端初始化）：
    /// sherpa 仅在启动时读热词文件，需重启加载聚合后的最新解码偏置（约 7s，期间语音短暂不可用）。
    /// </summary>
    public void ScheduleSherpaRestart(string reason)
    {
        if (Interlocked.Exchange(ref _restartPending, 1) == 1) return;   // 已有重启在排队，合并
        if (_sherpa == null) { Interlocked.Exchange(ref _restartPending, 0); return; }
        _ = Task.Run(async () =>
        {
            try
            {
                await Task.Delay(2000).ConfigureAwait(false);   // 等待同批次导入/刷新全部完成后再重启
                await _sherpa.RestartAsync().ConfigureAwait(false);
                _logger.LogInformation("[VOICE] sherpa-onnx 已重启加载聚合热词（{Reason}）", reason);
            }
            catch (Exception ex)
            {
                _logger.LogWarning(ex, "[VOICE] sherpa-onnx 重启失败（下次语音使用时会自动重试拉起）");
            }
            finally { Interlocked.Exchange(ref _restartPending, 0); }
        });
    }

    private VectorIndexData? LoadIndex(string key)
    {
        var path = CellIndexPath(key);
        if (!File.Exists(path)) return null;
        try { return VectorIndex.Load(path); }
        catch (Exception ex)
        {
            _logger.LogWarning(ex, "[TABLES] 加载索引失败 key={Key} path={Path}", key, path);
            return null;
        }
    }

    private string CellIndexDir(string key) => Path.Combine(_paths.TablesBaseDir, key);

    /// <summary>向量库二进制文件（VTX1）。不兼容旧 JSON——旧表用 /import_table 重新导入即生成新格式。</summary>
    private string CellIndexPath(string key) => Path.Combine(CellIndexDir(key), "cell_index.bin");

    private static void TryDeleteFile(string? path)
    {
        try { if (!string.IsNullOrEmpty(path) && File.Exists(path)) File.Delete(path); }
        catch (Exception) { /* 清理失败不影响主流程 */ }
    }
}
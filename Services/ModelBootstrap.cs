namespace VoiceTableAssist.Services;

/// <summary>解析后的模型目录路径集合（含多表目录，均相对 RANER_MODEL_DIR / exe 解析）。</summary>
internal sealed record ModelPaths(
    string ModelDir,
    string RanerDir,
    string EmbedDir,
    string TablesBaseDir,
    string HrBaseDir,
    string DefaultTable)
{
    public string RegistryPath => Path.Combine(TablesBaseDir, "registry.json");
    public string LegacyIndexPath => Path.Combine(EmbedDir, "cell_index.json");
}

/// <summary>
/// 解析模型目录并注册引擎宿主（EngineHost 按需加载 RaNER + gte-base-zh）。
/// 模型目录：优先 RANER_MODEL_DIR 覆盖，否则相对可执行目录解析 models/（raner + embedding）。
/// 启动只做文件存在性校验（缺文件立即失败）；引擎加载时机由 Models:LazyLoad 决定。
/// </summary>
internal static class ModelBootstrap
{
    public static void Register(IServiceCollection services, IConfiguration configuration)
    {
        var paths = ResolvePaths(configuration);
        CheckFiles(paths);

        Console.WriteLine($"[RANER] 模型目录 = {paths.ModelDir}");
        services.AddSingleton(paths);
        services.AddSingleton<EngineHost>();
        // EngineHost 作为 hosted service 启动空闲巡检；非懒加载模式（Models:LazyLoad=false）启动即加载。
        services.AddHostedService(sp => sp.GetRequiredService<EngineHost>());
    }

    /// <summary>非懒加载模式启动自检：校验 RaNER 抽取 + 向量定位链路。懒加载模式在首次加载后由 EngineHost 记录状态。</summary>
    public static void RunSelfCheck(IServiceProvider sp)
    {
        var host = sp.GetRequiredService<EngineHost>();
        if (!host.IsLoaded)
        {
            Console.WriteLine("[RANER] 懒加载模式：自检推迟到首次使用");
            return;
        }
        var embed = host.Embed;
        var tvm = sp.GetRequiredService<TableVectorManager>();

        Console.WriteLine($"[RANER] 活动表: {tvm.LastDatabase ?? "(无)"} 向量库 {(tvm.ActiveIndex?.RowsCount ?? 0)}行x{(tvm.ActiveIndex?.ColsCount ?? 0)}列 x{(tvm.ActiveIndex?.Dim ?? 0)}维 ({(tvm.ActiveIndex?.Entries.Count ?? 0)}向量)");

        var probe = host.Raner.Predict("外径一号十七点八四");
        var triples = TripleExtractor.Extract(probe);
        string? first = null;
        string? pc = null;
        if (triples.Count > 0)
        {
            var (sub, obj, val) = triples[0];
            var phrase = sub + obj;
            var hit = embed.Lookup(phrase, tvm.ActiveIndex);
            first = $"({hit.Row},{hit.Col}) sim={hit.Sim:F3} 命中=\"{hit.Phrase}\"";
            pc = phrase;
        }
        Console.WriteLine($"[RANER] 自检: \"外径一号十七点八四\" -> 三元组={triples.Count} {first ?? "未提取"} (query=\"{pc}\") 模型目录={sp.GetRequiredService<ModelPaths>().ModelDir}");
    }

    private static ModelPaths ResolvePaths(IConfiguration configuration)
    {
        // 模型根：RANER_MODEL_DIR 指向含 raner/, embedding/ 的目录；否则为 exe 目录下的 models/。
        var envModels = Environment.GetEnvironmentVariable("RANER_MODEL_DIR");
        var modelDir = string.IsNullOrWhiteSpace(envModels)
            ? Path.GetFullPath(Path.Combine(AppContext.BaseDirectory, "models"))
            : Path.GetFullPath(envModels);

        // 多表目录：向量库随模型根（embedding/tables）；语音资源 sherpa 独立于模型根。
        var tablesDir = ResolveRelative(configuration["Tables:BaseDir"], "embedding/tables", modelDir);
        var hrBase = ResolveRelative(configuration["Tables:HrBaseDir"], "sherpa-onnx/hr/tables", AppContext.BaseDirectory);

        var defaultTable = string.IsNullOrWhiteSpace(configuration["Tables:DefaultTable"]) ? "default" : configuration["Tables:DefaultTable"]!;

        return new ModelPaths(
            modelDir,
            Path.Combine(modelDir, "raner"),
            Path.Combine(modelDir, "embedding"),
            tablesDir,
            hrBase,
            defaultTable);
    }

    private static string ResolveRelative(string? cfg, string def, string baseDir)
    {
        var p = string.IsNullOrWhiteSpace(cfg) ? def : cfg!;
        return Path.IsPathRooted(p) ? p : Path.GetFullPath(Path.Combine(baseDir, p));
    }

    private static void CheckFiles(ModelPaths paths)
    {
        foreach (var f in new[]
        {
            Path.Combine(paths.RanerDir, "model.onnx"),
            Path.Combine(paths.RanerDir, "vocab.txt"),
            Path.Combine(paths.RanerDir, "crf_transitions.json"),
            Path.Combine(paths.EmbedDir, "model_quantized.onnx"),
            Path.Combine(paths.EmbedDir, "tokenizer.json"),
        })
            if (!File.Exists(f)) throw new FileNotFoundException($"模型资源缺失: {f}");
    }
}
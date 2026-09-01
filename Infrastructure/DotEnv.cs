namespace VoiceTableAssist.Infrastructure;

/// <summary>本地 .env 文件加载器，在 CreateBuilder 前将配置注入环境变量。</summary>
internal static class DotEnv
{
    public static void Load()
    {
        var current = Directory.GetCurrentDirectory();
        var candidates = new[]
        {
            Path.Combine(current, "app", ".env"),
            Path.Combine(current, ".env"),
            Path.Combine(AppContext.BaseDirectory, ".env")
        };

        var path = candidates.FirstOrDefault(File.Exists);
        if (path is null) return;

        foreach (var rawLine in File.ReadLines(path))
        {
            var line = rawLine.Trim();
            if (line.Length == 0 || line.StartsWith('#')) continue;

            var separator = line.IndexOf('=');
            if (separator <= 0) continue;

            var name = line[..separator].Trim();
            var value = line[(separator + 1)..].Trim();
            if (value.Length >= 2 && ((value[0] == '"' && value[^1] == '"') || (value[0] == '\'' && value[^1] == '\'')))
                value = value[1..^1];

            if (string.IsNullOrEmpty(Environment.GetEnvironmentVariable(name)))
                Environment.SetEnvironmentVariable(name, value);
        }
    }
}